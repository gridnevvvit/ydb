#include "authentication_options.h"

#include <yt/yt/core/rpc/authentication_identity.h>

#include <yt/yt/core/misc/error.h>

namespace NYT::NAuth {

////////////////////////////////////////////////////////////////////////////////

const std::string& TAuthenticationOptions::GetAuthenticatedUser() const
{
    static const std::string UnknownUser("<unknown>");
    return User ? *User : UnknownUser;
}

NRpc::TAuthenticationIdentity TAuthenticationOptions::GetAuthenticationIdentity() const
{
    if (!User) {
        THROW_ERROR_EXCEPTION("Authenticated user is not specified in client options");
    }
    return NRpc::TAuthenticationIdentity(*User, UserTag.value_or(*User));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
