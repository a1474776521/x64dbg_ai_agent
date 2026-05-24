// util/http_options.h
//
// 集中管理 cpr 请求级的 SSL/连接选项。
// 主要目的：在 Windows Schannel 后端下，企业网/离线网络无法访问 CRL/OCSP
// 吊销服务器时，TLS 握手会以 CRYPT_E_REVOCATION_OFFLINE (0x80092013) 失败。
// 通过 CURLSSLOPT_NO_REVOKE（NoRevoke=true）关闭吊销检查即可放行。
#pragma once

#include <chrono>

#include <cpr/cpr.h>

#include "util/config.h"

namespace x64ai {

// 默认 SSL 选项：关闭证书吊销检查（仅吊销，不影响证书链校验）。
inline cpr::SslOptions defaultSslOptions()
{
    return cpr::Ssl(cpr::ssl::NoRevoke{true});
}

// 流式请求总超时（毫秒）：长会话/推理可能持续数分钟，故大幅放宽。
inline cpr::Timeout streamTimeout()
{
    return cpr::Timeout{Config::instance().get().streamTimeoutMs};
}

// 流式低速断流阈值：N 秒内字节数低于 1 即中断，避免连接挂死时白等总超时。
inline cpr::LowSpeed streamLowSpeed()
{
    const int sec = Config::instance().get().streamLowSpeedSec;
    return cpr::LowSpeed{1, std::chrono::seconds(sec > 0 ? sec : 30)};
}

}  // namespace x64ai
