// ============================================================================
//  SRO Asset Studio - core/VpFormat.h
//
//  Constants that belong to the VSROPLUS container FORMAT itself (the vsroplus
//  toolkit), NOT to any particular server. Same category as the published
//  JMXPACK base keys (STD / PARADISE / ZERO) that SPEC section 2 already allows
//  to live in the binary: format-level, identical across every licensee.
//
//  Why they are needed: most shipped clients are protected, so their DLLs carry
//  no plaintext key material to harvest. Without these a .vp from such a client
//  could not be opened at all.
//
//  IMPORTANT: they are FALLBACKS, tried only after material discovered from the
//  client binaries next to the archive, so a re-keyed toolkit keeps working with
//  no code change. Nothing here is trusted until the container's own AES-GCM tag
//  proves it.
//
//  Verified against two independent servers (Electus v2.4, Legends Online v2.2).
// ============================================================================
#pragma once

#include <string>
#include "Obfuscate.h"

namespace sro { namespace vp {

/// RSA-2048 private key the toolkit embeds to wrap each container's AES key and
/// its password. Stored XOR-obfuscated (see Obfuscate.h): a `strings`/disasm
/// dump of the exe shows only noise where this PEM used to sit. Rebuilt at run
/// time into a std::string, byte-for-byte identical to the original literal.
inline std::string ToolkitRsaPem() {
    return
        std::string(OBF("-----BEGIN RSA PRIVATE KEY-----\n").str()) +
        OBF("MIIEpQIBAAKCAQEAmu2erClUwXzYKT6IAyebfUspbUNILiS/+ZoGzy4nWiYtEyOD\n").str() +
        OBF("Fi1JNi1XdNTWHaAnGX2QWw6TAd9ljt3W33KTkuciam+RVpiC7tLRMx8YbFEDTkpJ\n").str() +
        OBF("sW0M2PjKUMK9+/je+ncxrS788LHCq34jPtRx1GpOKrrfj/OzjviEPpalrko3Al9p\n").str() +
        OBF("4nVxvMbkm9dMH68rw7Flc8lQ39QG1DEzeYMzK5nSv7DDh4dMoXwh1Qbi+4VUCcWr\n").str() +
        OBF("YMZKYcCKK+ZiB+xR3F+zvthFxEA5rwTnCUOTEHOYPWRZENZYDgj1l+gC1yCqxlxA\n").str() +
        OBF("iPMv880mtlkl1ctSUXDhW0inK36pFtyefFVSgwIDAQABAoIBABxyHp0bpZ01D/da\n").str() +
        OBF("rNxvbEQwBbYzEVCvnLP2jOODZORPeB04evwaaoPRBWrFHV8pJaSvgk7FuUYxv1Lg\n").str() +
        OBF("9m/meKQTXiVOwM+2huvUt2Z8gh55LXM1fpxeGmrIG1WCvCFEi6RPOEbFBtomuUiF\n").str() +
        OBF("i3EgzpmivyFnWF707vYEOTvIqzj5yylCtRWGtB+zOGIgx+9z4V0L/l446KVMv/xn\n").str() +
        OBF("ZnvrQU9OpWy7VLHT27AUNR6ZywPgY8SIJRxD5A8RJCiDOlaJvpJc4Llk5mbxuNf7\n").str() +
        OBF("fYETB1HI0Ku7ImtSvOMbqCZDcTf1WTCMtia8H1gWvhbknX94V9W1HZrhOE3vXBNb\n").str() +
        OBF("Hie06RECgYEA7TCwQOVuSXpOQ1HV0B+3rj+kN0Aquk4iG27ROSyPBVgLa5fdZMHE\n").str() +
        OBF("+cbLb1Ux+S2FPResNxH+q6AH4xwR7AWe5FT9IC++WaQm9RshR1JqtBBiYT3H0dzc\n").str() +
        OBF("i/+lCG0J/h1vzEfGAxX1w7JF5ViEdmQkHKw1Of0ywFGSixudL4H92osCgYEApzbk\n").str() +
        OBF("MOBaoDhF852BiUU4YecUBasitShrMicd1jC/u1A013UUa2gIHYp3Dz2Ki2/D+XtS\n").str() +
        OBF("7N0LZRThXikNvGmRlw4WdAYe4jxkPrDbvemY+XLn6SRryMiZLwI7GDVHgpQmaM3m\n").str() +
        OBF("yvsqDkfS56MWLqTzPULAvhFfS8aXNYR3YBxLfukCgYEAuZ3roK2rdDcRfh9RuvDH\n").str() +
        OBF("VsaZJV/C0E+K43sCosKbHix7q3SKHmqBUpLe6z55wC1+YoPfEq2zZe3znih+Yf04\n").str() +
        OBF("DinweTrDtPUAQ7o6fc0ikc1sn/qwICy+L5UdOo3STxoMPopAjHOoA8DEcCmoZ9oQ\n").str() +
        OBF("ZJvjf4EFMXnku5YdmWsaFOcCgYEAoBIC9er/tlrIVAKWa2LxK397AkYmTodnvrYF\n").str() +
        OBF("cPJvyI9qVksFBiCrjCPt/4/pHbwbPatZ4ddkvhnz+CCSvqfMsG7S/bSSQf6P9Bd3\n").str() +
        OBF("8+A4Cat+6AduiuHFCmQ3T+BJGW5bKn7ZDbzMMKl+8zGC3ZoYI6/QNOMKd86fF6M8\n").str() +
        OBF("Z6wOdWkCgYEAtEvu/k2379rAkv+Jm+J1yqE4Vmag8VXyAkuGoHkFsUoZ3Y9aEXSw\n").str() +
        OBF("VkCTTiwa2zU6cvC4nbD0jt0QMgoXb238LVB/3hhGs6vLPTBuvlnj57JCUMkKa8eT\n").str() +
        OBF("UuI66k6M1z7nEKSQWXAa0jG37Ja+F9azjfLd6ohfexwbQ7Rl+vGGxb4=\n").str() +
        OBF("-----END RSA PRIVATE KEY-----\n").str();
}

/// The L33T variant ("L33T ENCRYPTED CONTAINER", VERSION 1.0) is the same
/// container family with the same crypto chain, but ships its own RSA keypair.
/// This private key PKCS#1-v1.5-unwraps every L33T blob1 to that container's
/// perLicenseKey (verified against all six DESTEK containers). Format-level
/// fallback, exactly like the toolkit key above; the AES-GCM tag still proves it.
inline std::string L33tRsaPem() {
    return
        std::string(OBF("-----BEGIN RSA PRIVATE KEY-----\n").str()) +
        OBF("MIIEpAIBAAKCAQEAqtekLU0aRF32xAs2SoYu2UKRK6LSEgsyblIWPkeB/PTjWG4D\n").str() +
        OBF("PaxW61Ul6yQPm0rjydfwS4yokLtSpNB6I00YONV//cDuv+JdIwdxzeO4vcoP7/4A\n").str() +
        OBF("AYIF2crw1u8aBdWptd4TCpfhT+gZt21gNA7p2g/hekbtgBJUHAOdttE+l5uATNir\n").str() +
        OBF("t8zPnTyx7PhXoDUyJABmoG5gD1lhtDutddYGb57AfcWyASnZJEcjX95tDy3hypSj\n").str() +
        OBF("XSGwUZS42Qf7e50adN5IHwd1BrbdvXH5Jt9eg8d+T1PXlE+b606hSi77eRBAlv52\n").str() +
        OBF("CqthmEq9IcPwXdK/ZwPBlfB2VOdLxJ3v3T/SrQIDAQABAoIBAEMPZF1333CopZ1s\n").str() +
        OBF("fUIhi7AyZgOH5Qe6J3CQfjSyIv8lLTxf6pvB01iK0tiLDik561ImlidBrkUpkpYp\n").str() +
        OBF("U1gaAgEFJTl5Cv59H2wJlw1nbvPopfrhbCUrYZqNA56Kul9B8Fz4L+c/mOyhwb5z\n").str() +
        OBF("i4jDtd2n0C0MPFvCVgutC7JgTj554j838mBUbMramGjP7UeVuEYucxa+PzKPUqVM\n").str() +
        OBF("po7C053ofL2yAZa9+KTc2p8DdA6fTWXjz2Dbix+TtsexwXw3JCbjO4KGAzesI1dB\n").str() +
        OBF("QeEJPPDdI5ogU7A86vrm66UIhLJPcEZdVKJnZYbcPK6mm0HEKacbWx14gxRe99uw\n").str() +
        OBF("yw0FHmECgYEAyKEpkB+ZAez+FpSr4qdlFy1S9Bd3Q974fx6DjixOy4EaPYCG56P6\n").str() +
        OBF("rM8MrsU4sfJpLrWrWGUZPGf4JSNmM6h8jx2GdIZ2vy0Oz0DFHGUF92R0TwR6dPZa\n").str() +
        OBF("yPVVQvMAshrBD0Ih48Y302jDIxIOTeA8T9o+ViB2t5YZ0SFd9yZ+l/UCgYEA2f31\n").str() +
        OBF("2LA/N23qIG5Wcn0qqiAjSg+Ld2ArfmXXDXhYhkQ6YxplPWos3FC3JE63pxbiLOep\n").str() +
        OBF("AQsNCmL4fp5Pewo4ys+cqpvIygPRKm05hi/e75/0wE06bubBO9m5GMzxtIHI5w5e\n").str() +
        OBF("p/vnwjqeL6RhdOjPVW9I+aV3CGglMs1HNXL+dNkCgYEAqljUbm+1vYHY95lCHE8f\n").str() +
        OBF("MT+liGUIOdNu0Jg7ARMzz+yNOB0R7ko3xFSEozwHeGen0SYlFwe4Fqb8daQJ2eC7\n").str() +
        OBF("6dQa3oNw/b+nLGgFNKe+GiDr9E+MHhUyXiCYRSEIvTCkLW1sUvZxXgo8bWGeTZcP\n").str() +
        OBF("/okClqQ3yIck4oFfREMmB6UCgYAtvGC2Q9QflwoGH+A3SReIdhGxWi2SsDSv4kIK\n").str() +
        OBF("a3N3SB1P6ZvQFNSwrKagEXvqrJxre3cz5tOnmDg2qJRYZj6gHorqKKEp8T8TPbNR\n").str() +
        OBF("+f02HUNGjW1zEyr7maSCFY2op4o1IAiwCeQIFjBWIpDn7GiDrkOLTTH6K4+SZwtH\n").str() +
        OBF("pYbJkQKBgQCMj7HVqFUQBFkHH6xaEddIiqXKqJsJzt4eCkPYjNOaUemypS64Rk5Y\n").str() +
        OBF("GEJ74IZNgOFYV+RE7NJ/+Wt4cmOJKGyCYIsGxrvtroP2pH66g/mn4Smu29vj1aLT\n").str() +
        OBF("2bIdQF8/Glm3asG5Zs+QdPmzwdZbal61KFYwqq5vFImBsGO6SYCVcw==\n").str() +
        OBF("-----END RSA PRIVATE KEY-----\n").str();
}

/// AES-256-ECB keystream key used by File_EncryptDecrypt for payload
/// obfuscation (containers with IsFullEncrypted, or per-file IsEncrypted).
/// Obfuscated at rest (Obfuscate.h) so the raw key never sits in the exe.
inline std::string ToolkitXorKey() { return OBF("7C767271FD1867AAC5956724523D5AWG").str(); }

/// KDF context for the metadata signing key:
/// signKey = HMAC-SHA256(metadataKey, SignContext()).
inline std::string SignContext()   { return OBF("QWGUonhWmdiMSnJKYKGtNd1aBjy2pXP7").str(); }

}} // namespace sro::vp
