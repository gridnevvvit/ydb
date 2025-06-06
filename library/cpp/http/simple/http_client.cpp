#include "http_client.h"

#include <library/cpp/string_utils/url/url.h>

#include <util/stream/output.h>
#include <util/string/cast.h>
#include <util/string/join.h>
#include <util/string/split.h>
#include <util/system/spinlock.h>
#include <library/cpp/cache/cache.h>


TSpinLock TKeepAliveHttpClient::ConnectionQuarantineMutex;
TQueue<THolder<NPrivate::THttpConnection>> TKeepAliveHttpClient::ConnectionQuarantine;

TKeepAliveHttpClient::TKeepAliveHttpClient(const TString& host,
                                           ui32 port,
                                           TDuration socketTimeout,
                                           TDuration connectTimeout,
                                           bool useKeepAlive,
                                           bool useConnectionPool)
    : Host(CutHttpPrefix(host))
    , Port(port)
    , SocketTimeout(socketTimeout)
    , ConnectTimeout(connectTimeout)
    , UseKeepAlive(useKeepAlive)
    , UseConnectionPool(useConnectionPool)
    , IsHttps(host.StartsWith("https"))
    , IsClosingRequired(false)
    , HttpsVerification(TVerifyCert{Host})
    , IfResponseRequired([](const THttpInput&) { return true; })
{
}

TKeepAliveHttpClient::THttpCode TKeepAliveHttpClient::DoGet(const TStringBuf relativeUrl,
                                                            IOutputStream* output,
                                                            const THeaders& headers,
                                                            THttpHeaders* outHeaders,
                                                            NThreading::TCancellationToken cancellation) {
    return DoRequest(TStringBuf("GET"),
                     relativeUrl,
                     {},
                     output,
                     headers,
                     outHeaders,
                     std::move(cancellation));
}

TKeepAliveHttpClient::THttpCode TKeepAliveHttpClient::DoPost(const TStringBuf relativeUrl,
                                                             const TStringBuf body,
                                                             IOutputStream* output,
                                                             const THeaders& headers,
                                                             THttpHeaders* outHeaders,
                                                             NThreading::TCancellationToken cancellation) {
    return DoRequest(TStringBuf("POST"),
                     relativeUrl,
                     body,
                     output,
                     headers,
                     outHeaders,
                     std::move(cancellation));
}

TKeepAliveHttpClient::THttpCode TKeepAliveHttpClient::DoRequest(const TStringBuf method,
                                                                const TStringBuf relativeUrl,
                                                                const TStringBuf body,
                                                                IOutputStream* output,
                                                                const THeaders& inHeaders,
                                                                THttpHeaders* outHeaders,
                                                                NThreading::TCancellationToken cancellation) {
    const TString contentLength = IntToString<10, size_t>(body.size());
    return DoRequestReliable(FormRequest(method, relativeUrl, body, inHeaders, contentLength), output, outHeaders, std::move(cancellation));
}

TKeepAliveHttpClient::THttpCode TKeepAliveHttpClient::DoRequestRaw(const TStringBuf raw,
                                                                   IOutputStream* output,
                                                                   THttpHeaders* outHeaders,
                                                                   NThreading::TCancellationToken cancellation) {
    return DoRequestReliable(raw, output, outHeaders, std::move(cancellation));
}

void TKeepAliveHttpClient::DisableVerificationForHttps() {
    HttpsVerification.Clear();
    Connection.Reset();
}

void TKeepAliveHttpClient::SetClientCertificate(const TOpenSslClientIO::TOptions::TClientCert& options) {
    ClientCertificate = options;
}

void TKeepAliveHttpClient::ResetConnection() {
    Connection.Reset();
}

TVector<IOutputStream::TPart> TKeepAliveHttpClient::FormRequest(TStringBuf method,
                                                                const TStringBuf relativeUrl,
                                                                TStringBuf body,
                                                                const TKeepAliveHttpClient::THeaders& headers,
                                                                TStringBuf contentLength) const {
    TVector<IOutputStream::TPart> parts;

    parts.reserve(16 + 4 * headers.size());
    parts.push_back(method);
    parts.push_back(TStringBuf(" "));
    parts.push_back(relativeUrl);
    parts.push_back(TStringBuf(" HTTP/1.1"));
    parts.push_back(IOutputStream::TPart::CrLf());
    parts.push_back(TStringBuf("Host: "));
    parts.push_back(TStringBuf(Host));
    parts.push_back(IOutputStream::TPart::CrLf());
    parts.push_back(TStringBuf("Content-Length: "));
    parts.push_back(contentLength);
    parts.push_back(IOutputStream::TPart::CrLf());

    for (const auto& entry : headers) {
        parts.push_back(IOutputStream::TPart(entry.first));
        parts.push_back(IOutputStream::TPart(TStringBuf(": ")));
        parts.push_back(IOutputStream::TPart(entry.second));
        parts.push_back(IOutputStream::TPart::CrLf());
    }

    parts.push_back(IOutputStream::TPart::CrLf());
    if (body) {
        parts.push_back(IOutputStream::TPart(body));
    }

    return parts;
}

TKeepAliveHttpClient::THttpCode TKeepAliveHttpClient::ReadAndTransferHttp(THttpInput& input,
                                                                          IOutputStream* output,
                                                                          THttpHeaders* outHeaders) const {
    TKeepAliveHttpClient::THttpCode statusCode;
    try {
        statusCode = ParseHttpRetCode(input.FirstLine());
    } catch (TFromStringException& e) {
        TString rest = input.ReadAll();
        ythrow THttpRequestException() << "Failed parse status code in response of " << Host << ": " << e.what() << " (" << input.FirstLine() << ")"
                                       << "\nFull http response:\n"
                                       << rest;
    }

    auto canContainBody = [](auto statusCode) {
        return statusCode != HTTP_NOT_MODIFIED && statusCode != HTTP_NO_CONTENT;
    };

    if (output && canContainBody(statusCode) && IfResponseRequired(input)) {
        TransferData(&input, output);
    }
    if (outHeaders) {
        *outHeaders = input.Headers();
    }

    return statusCode;
}

THttpInput* TKeepAliveHttpClient::GetHttpInput() {
    return Connection ? Connection->GetHttpInput() : nullptr;
}

bool TKeepAliveHttpClient::CreateNewConnectionIfNeeded() {
    if (IsClosingRequired || (Connection && !Connection->IsOk())) {
        Connection.Reset();
    }
    if (!Connection) {
        Connection = MakeHolder<NPrivate::THttpConnection>(Host,
                                                           Port,
                                                           SocketTimeout,
                                                           ConnectTimeout,
                                                           IsHttps,
                                                           ClientCertificate,
                                                           HttpsVerification,
                                                           UseKeepAlive);
        IsClosingRequired = false;
        return true;
    }
    return false;
}

TKeepAliveHttpClient::~TKeepAliveHttpClient() {
    if (UseConnectionPool) {
        THolder<NPrivate::THttpConnection> oldConnection;
        with_lock(ConnectionQuarantineMutex) {
            while (ConnectionQuarantine.size() > 100) {
                oldConnection = std::move(ConnectionQuarantine.front());

                ConnectionQuarantine.pop();
                oldConnection.Reset();
            }
            ConnectionQuarantine.push(std::move(Connection));
        }
    }
}

THttpRequestException::THttpRequestException(int statusCode)
    : StatusCode(statusCode)
{
}

int THttpRequestException::GetStatusCode() const {
    return StatusCode;
}

TSimpleHttpClient::TSimpleHttpClient(const TOptions& options)
    : Host(options.Host())
    , Port(options.Port())
    , SocketTimeout(options.SocketTimeout())
    , ConnectTimeout(options.ConnectTimeout())
    , UseKeepAlive(options.UseKeepAlive())
    , UseConnectionPool(options.UseConnectionPool())
{
}

TSimpleHttpClient::TSimpleHttpClient(const TString& host, ui32 port, TDuration socketTimeout, TDuration connectTimeout)
    : Host(host)
    , Port(port)
    , SocketTimeout(socketTimeout)
    , ConnectTimeout(connectTimeout)
{
}

void TSimpleHttpClient::EnableVerificationForHttps() {
    HttpsVerification = true;
}

void TSimpleHttpClient::DoGet(const TStringBuf relativeUrl, IOutputStream* output, const THeaders& headers, THttpHeaders* outHeaders, NThreading::TCancellationToken cancellation) const {
    TKeepAliveHttpClient cl = CreateClient();

    TKeepAliveHttpClient::THttpCode code = cl.DoGet(relativeUrl, output, headers, outHeaders, std::move(cancellation));

    Y_ENSURE(cl.GetHttpInput());
    ProcessResponse(relativeUrl, *cl.GetHttpInput(), output, code);
}

void TSimpleHttpClient::DoPost(const TStringBuf relativeUrl, TStringBuf body, IOutputStream* output, const THashMap<TString, TString>& headers, THttpHeaders* outHeaders, NThreading::TCancellationToken cancellation) const {
    TKeepAliveHttpClient cl = CreateClient();

    TKeepAliveHttpClient::THttpCode code = cl.DoPost(relativeUrl, body, output, headers, outHeaders, std::move(cancellation));

    Y_ENSURE(cl.GetHttpInput());
    ProcessResponse(relativeUrl, *cl.GetHttpInput(), output, code);
}

void TSimpleHttpClient::DoPostRaw(const TStringBuf relativeUrl, const TStringBuf rawRequest, IOutputStream* output, THttpHeaders* outHeaders, NThreading::TCancellationToken cancellation) const {
    TKeepAliveHttpClient cl = CreateClient();

    TKeepAliveHttpClient::THttpCode code = cl.DoRequestRaw(rawRequest, output, outHeaders, std::move(cancellation));

    Y_ENSURE(cl.GetHttpInput());
    ProcessResponse(relativeUrl, *cl.GetHttpInput(), output, code);
}

namespace NPrivate {
    THttpConnection::THttpConnection(const TString& host,
                                     ui32 port,
                                     TDuration sockTimeout,
                                     TDuration connTimeout,
                                     bool isHttps,
                                     const TMaybe<TOpenSslClientIO::TOptions::TClientCert>& clientCert,
                                     const TMaybe<TOpenSslClientIO::TOptions::TVerifyCert>& verifyCert,
                                     bool keepAlive)
        : Addr(Resolve(host, port))
        , Socket(Connect(Addr, sockTimeout, connTimeout, host, port))
        , SocketIn(Socket)
        , SocketOut(Socket)
    {
        if (isHttps) {
            TOpenSslClientIO::TOptions opts;
            if (clientCert) {
                opts.ClientCert_ = clientCert;
            }
            if (verifyCert) {
                opts.VerifyCert_ = verifyCert;
            }

            Ssl = MakeHolder<TOpenSslClientIO>(&SocketIn, &SocketOut, opts);
            HttpOut = MakeHolder<THttpOutput>(Ssl.Get());
        } else {
            HttpOut = MakeHolder<THttpOutput>(&SocketOut);
        }
        if (keepAlive) {
            HttpOut->EnableKeepAlive(true);
        }
    }

    TNetworkAddress THttpConnection::Resolve(const TString& host, ui32 port) {
        try {
            return TNetworkAddress(host, port);
        } catch (const yexception& e) {
            ythrow THttpRequestException() << "Resolve of " << host << ": " << e.what();
        }
    }

    TSocket THttpConnection::Connect(TNetworkAddress& addr,
                                     TDuration sockTimeout,
                                     TDuration connTimeout,
                                     const TString& host,
                                     ui32 port) {
        try {
            TSocket socket(addr, connTimeout);
            TDuration socketTimeout = Max(sockTimeout, TDuration::MilliSeconds(1)); // timeout less than 1ms will be interpreted as 0 in SetSocketTimeout() call below and will result in infinite wait

            ui32 seconds = socketTimeout.Seconds();
            ui32 milliSeconds = (socketTimeout - TDuration::Seconds(seconds)).MilliSeconds();
            socket.SetSocketTimeout(seconds, milliSeconds);
            return socket;
        } catch (const yexception& e) {
            ythrow THttpRequestException() << "Connect to " << host << ':' << port << " failed: " << e.what();
        }
    }
}

void TSimpleHttpClient::ProcessResponse(const TStringBuf relativeUrl, THttpInput& input, IOutputStream*, const unsigned statusCode) const {
    if (!(statusCode >= 200 && statusCode < 300)) {
        TString rest = input.ReadAll();
        ythrow THttpRequestException(statusCode) << "Got " << statusCode << " at " << Host << relativeUrl << "\nFull http response:\n"
                                                 << rest;
    }
}

TSimpleHttpClient::~TSimpleHttpClient() {
}

TKeepAliveHttpClient TSimpleHttpClient::CreateClient() const {
    TKeepAliveHttpClient cl(Host, Port, SocketTimeout, ConnectTimeout, UseKeepAlive, UseConnectionPool);

    if (!HttpsVerification) {
        cl.DisableVerificationForHttps();
    }

    PrepareClient(cl);

    return cl;
}

void TSimpleHttpClient::PrepareClient(TKeepAliveHttpClient&) const {
}

TRedirectableHttpClient::TRedirectableHttpClient(const TOptions& options)
    : TSimpleHttpClient(options)
    , Opts(options)
{
}

TRedirectableHttpClient::TRedirectableHttpClient(const TString& host, ui32 port, TDuration socketTimeout, TDuration connectTimeout)
    : TRedirectableHttpClient(TOptions().Host(host).Port(port).SocketTimeout(socketTimeout).ConnectTimeout(connectTimeout))
{
}

void TRedirectableHttpClient::PrepareClient(TKeepAliveHttpClient& cl) const {
    cl.IfResponseRequired = [](const THttpInput& input) {
        return !input.Headers().HasHeader("Location");
    };
}

void TRedirectableHttpClient::ProcessResponse(const TStringBuf relativeUrl, THttpInput& input, IOutputStream* output, const unsigned statusCode) const {
    for (auto i = input.Headers().Begin(), e = input.Headers().End(); i != e; ++i) {
        if (0 == TString::compare(i->Name(), TStringBuf("Location"))) {
            if (Opts.MaxRedirectCount() == 0) {
                ythrow THttpRequestException(statusCode) << "Exceeds MaxRedirectCount limit, code " << statusCode << " at " << Host << relativeUrl;
            }

            TStringBuf schemeHostPort = GetSchemeHostAndPort(i->Value());
            TStringBuf scheme("http://");
            TStringBuf host("unknown");
            ui16 port = 0;
            GetSchemeHostAndPort(schemeHostPort, scheme, host, port);
            TStringBuf body = GetPathAndQuery(i->Value(), false);
            if (port == 0) {
                if (scheme.StartsWith("https")) {
                    port = 443;
                } else if (scheme.StartsWith("http")) {
                    port = 80;
                } else {
                    port = 80;
                }
            }

            auto opts = Opts;
            opts.Host(TString(scheme) + TString(host));
            opts.Port(port);
            opts.MaxRedirectCount(opts.MaxRedirectCount() - 1);

            TRedirectableHttpClient cl(opts);
            if (HttpsVerification) {
                cl.EnableVerificationForHttps();
            }
            cl.DoGet(body, output);
            return;
        }
    }
    if (!(statusCode >= 200 && statusCode < 300)) {
        TString rest = input.ReadAll();
        ythrow THttpRequestException(statusCode) << "Got " << statusCode << " at " << Host << relativeUrl << "\nFull http response:\n"
                                                 << rest;
    }
    TransferData(&input, output);
}
