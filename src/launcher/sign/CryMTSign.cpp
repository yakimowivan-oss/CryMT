// CryMTSign.exe: the release signing key (ECDSA P-256, Windows CNG) for CryMT updates.
//   CryMTSign keygen <private key file> <public key header>   new key pair; the header goes into Launcher.cpp
//   CryMTSign sign <private key file> <file> <signature file>  signs the SHA-256 of the file (64 bytes r|s)
//   CryMTSign verify <private key file> <file> <signature file>
//   CryMTSign sha256 <file>
// The private key never goes into a repository or a package: whoever has it can sign updates every launcher installs.
#define _WIN32_WINNT 0x0A00 // BCryptHash
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
bool ReadAll(const wchar_t* path, std::vector<unsigned char>& data)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return false;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool WriteAll(const wchar_t* path, const void* data, size_t size)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return false;
    const bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool Sha256(const std::vector<unsigned char>& data, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    const bool ok = BCryptHash(alg, nullptr, 0, const_cast<unsigned char*>(data.data()), static_cast<ULONG>(data.size()), out, 32) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

BCRYPT_KEY_HANDLE ImportPrivate(BCRYPT_ALG_HANDLE alg, const wchar_t* path)
{
    std::vector<unsigned char> blob;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ReadAll(path, blob) ||
        BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()), 0) != 0)
        return nullptr;
    return key;
}

int Fail(const char* what)
{
    fprintf(stderr, "CryMTSign: %s\n", what);
    return 1;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
        return Fail("usage: keygen <key> <header> | sign <key> <file> <sig> | verify <key> <file> <sig> | sha256 <file>");
    const std::wstring cmd = argv[1];
    if (cmd == L"sha256")
    {
        std::vector<unsigned char> data;
        unsigned char hash[32];
        if (!ReadAll(argv[2], data) || !Sha256(data, hash))
            return Fail("cannot read or hash the file");
        for (unsigned char b : hash)
            printf("%02x", b);
        printf("\n");
        return 0;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
        return Fail("no ECDSA P-256 provider");
    if (cmd == L"keygen" && argc == 4)
    {
        if (GetFileAttributesW(argv[2]) != INVALID_FILE_ATTRIBUTES)
            return Fail("the key file exists already; not overwritten");
        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCryptGenerateKeyPair(alg, &key, 256, 0) != 0 || BCryptFinalizeKeyPair(key, 0) != 0)
            return Fail("key generation failed");
        ULONG size = 0;
        BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &size, 0);
        std::vector<unsigned char> priv(size);
        BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, priv.data(), size, &size, 0);
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size, 0);
        std::vector<unsigned char> pub(size);
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, pub.data(), size, &size, 0);
        std::string header = "// CryMT release key (public part, BCRYPT_ECCPUBLIC_BLOB, ECDSA P-256), made by CryMTSign keygen.\n"
                             "static const unsigned char kUpdateKey[] = {";
        for (size_t i = 0; i < pub.size(); i++)
        {
            char hex[8];
            snprintf(hex, sizeof(hex), "%s0x%02x", i ? ", " : "", pub[i]);
            header += (i % 16 == 0 ? "\n    " : "") + std::string(hex);
        }
        header += "};\n";
        if (!WriteAll(argv[2], priv.data(), priv.size()) || !WriteAll(argv[3], header.data(), header.size()))
            return Fail("cannot write the key files");
        printf("key pair written: private %zu bytes, public %zu bytes\n", priv.size(), pub.size());
        return 0;
    }
    if ((cmd == L"sign" || cmd == L"verify") && argc == 5)
    {
        BCRYPT_KEY_HANDLE key = ImportPrivate(alg, argv[2]);
        std::vector<unsigned char> data;
        unsigned char hash[32];
        if (!key)
            return Fail("cannot read the private key");
        if (!ReadAll(argv[3], data) || !Sha256(data, hash))
            return Fail("cannot read the file");
        if (cmd == L"sign")
        {
            unsigned char sig[64];
            ULONG size = 0;
            if (BCryptSignHash(key, nullptr, hash, 32, sig, sizeof(sig), &size, 0) != 0 || size != 64)
                return Fail("signing failed");
            if (!WriteAll(argv[4], sig, 64))
                return Fail("cannot write the signature");
            printf("signed: %zu bytes\n", data.size());
            return 0;
        }
        std::vector<unsigned char> sig;
        if (!ReadAll(argv[4], sig) || sig.size() != 64)
            return Fail("cannot read the signature");
        const bool ok = BCryptVerifySignature(key, nullptr, hash, 32, sig.data(), 64, 0) == 0;
        printf("%s\n", ok ? "signature OK" : "signature BAD");
        return ok ? 0 : 2;
    }
    return Fail("unknown command or wrong arguments");
}
