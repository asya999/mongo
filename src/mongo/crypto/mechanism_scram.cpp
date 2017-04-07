/*
 *    Copyright (C) 2014 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/crypto/mechanism_scram.h"

#include <vector>

#include "mongo/crypto/crypto.h"
#include "mongo/platform/random.h"
#include "mongo/util/base64.h"
#include "mongo/util/secure_zero_memory.h"

namespace mongo {
namespace scram {

using std::unique_ptr;

namespace {
/**
 * Compare two arrays of bytes for equality in constant time.
 *
 * This means that the function runs for the same amount of time even if they differ. Unlike memcmp,
 * this function does not exit on the first difference.
 *
 * Returns true if the two arrays are equal.
 *
 * TODO: evaluate if LTO inlines or changes the code flow of this function.
 */
NOINLINE_DECL
bool consttimeMemEqual(volatile const unsigned char* s1,  // NOLINT - using volatile to
                       volatile const unsigned char* s2,  // NOLINT - disable compiler optimizations
                       size_t length) {
    unsigned int ret = 0;

    for (size_t i = 0; i < length; ++i) {
        ret |= s1[i] ^ s2[i];
    }

    return (1 & ((ret - 1) >> 8));
}
}  // namespace

std::string hashToBase64(const SecureHandle<SHA1Hash>& hash) {
    return base64::encode(reinterpret_cast<const char*>(hash->data()), hash->size());
}

// Compute the SCRAM step Hi() as defined in RFC5802
static SHA1Hash HMACIteration(const unsigned char input[],
                              size_t inputLen,
                              const unsigned char salt[],
                              size_t saltLen,
                              unsigned int iterationCount) {
    SHA1Hash output;
    SHA1Hash intermediateDigest;
    // Reserve a 20 byte block for the initial key. We use 16 byte salts, and must reserve an extra
    // 4 bytes for a suffix mandated by RFC5802.
    std::array<std::uint8_t, 20> startKey;

    uassert(17450, "invalid salt length provided", saltLen + 4 == startKey.size());
    std::copy(salt, salt + saltLen, startKey.begin());

    startKey[saltLen] = 0;
    startKey[saltLen + 1] = 0;
    startKey[saltLen + 2] = 0;
    startKey[saltLen + 3] = 1;

    // U1 = HMAC(input, salt + 0001)
    output = crypto::hmacSha1(input, inputLen, startKey.data(), startKey.size());
    intermediateDigest = output;

    // intermediateDigest contains Ui and output contains the accumulated XOR:ed result
    for (size_t i = 2; i <= iterationCount; i++) {
        intermediateDigest =
            crypto::hmacSha1(input, inputLen, intermediateDigest.data(), intermediateDigest.size());
        for (size_t k = 0; k < output.size(); k++) {
            output[k] ^= intermediateDigest[k];
        }
    }

    return output;
}

// Iterate the hash function to generate SaltedPassword
SHA1Hash generateSaltedPassword(const SCRAMPresecrets& presecrets) {
    // saltedPassword = Hi(hashedPassword, salt)
    SHA1Hash saltedPassword =
        HMACIteration(reinterpret_cast<const unsigned char*>(presecrets.hashedPassword.c_str()),
                      presecrets.hashedPassword.size(),
                      presecrets.salt.data(),
                      presecrets.salt.size(),
                      presecrets.iterationCount);

    return saltedPassword;
}

SCRAMSecrets generateSecrets(const SCRAMPresecrets& presecrets) {
    SHA1Hash saltedPassword = generateSaltedPassword(presecrets);
    return generateSecrets(saltedPassword);
}

SCRAMSecrets generateSecrets(const SHA1Hash& saltedPassword) {
    SCRAMSecrets credentials;

    // ClientKey := HMAC(saltedPassword, "Client Key")
    credentials.clientKey =
        crypto::hmacSha1(saltedPassword.data(),
                         saltedPassword.size(),
                         reinterpret_cast<const unsigned char*>(clientKeyConst.data()),
                         clientKeyConst.size());

    // StoredKey := H(clientKey)
    credentials.storedKey =
        crypto::sha1(credentials.clientKey->data(), credentials.clientKey->size());

    // ServerKey       := HMAC(SaltedPassword, "Server Key")
    credentials.serverKey =
        crypto::hmacSha1(saltedPassword.data(),
                         saltedPassword.size(),
                         reinterpret_cast<const unsigned char*>(serverKeyConst.data()),
                         serverKeyConst.size());

    return credentials;
}


BSONObj generateCredentials(const std::string& hashedPassword, int iterationCount) {
    const int saltLenQWords = 2;

    // Generate salt
    uint64_t userSalt[saltLenQWords];

    unique_ptr<SecureRandom> sr(SecureRandom::create());

    userSalt[0] = sr->nextInt64();
    userSalt[1] = sr->nextInt64();
    std::string encodedUserSalt =
        base64::encode(reinterpret_cast<char*>(userSalt), sizeof(userSalt));

    // Compute SCRAM secrets serverKey and storedKey
    auto secrets = generateSecrets(
        SCRAMPresecrets(hashedPassword,
                        std::vector<std::uint8_t>(reinterpret_cast<std::uint8_t*>(userSalt),
                                                  reinterpret_cast<std::uint8_t*>(userSalt) +
                                                      saltLenQWords * sizeof(uint64_t)),
                        iterationCount));

    std::string encodedStoredKey = hashToBase64(secrets.storedKey);
    std::string encodedServerKey = hashToBase64(secrets.serverKey);

    return BSON(iterationCountFieldName << iterationCount << saltFieldName << encodedUserSalt
                                        << storedKeyFieldName
                                        << encodedStoredKey
                                        << serverKeyFieldName
                                        << encodedServerKey);
}

std::string generateClientProof(const SCRAMSecrets& clientCredentials,
                                const std::string& authMessage) {
    // ClientSignature := HMAC(StoredKey, AuthMessage)
    SHA1Hash clientSignature =
        crypto::hmacSha1(clientCredentials.storedKey->data(),
                         clientCredentials.storedKey->size(),
                         reinterpret_cast<const unsigned char*>(authMessage.c_str()),
                         authMessage.size());

    // ClientProof   := ClientKey XOR ClientSignature
    SHA1Hash clientProof;
    for (size_t i = 0; i < clientCredentials.clientKey->size(); i++) {
        clientProof[i] = (*clientCredentials.clientKey)[i] ^ clientSignature[i];
    }

    return hashToBase64(clientProof);
}

bool verifyServerSignature(const SCRAMSecrets& clientCredentials,
                           const std::string& authMessage,
                           const std::string& receivedServerSignature) {
    // ServerSignature := HMAC(ServerKey, AuthMessage)
    SHA1Hash serverSignature =
        crypto::hmacSha1(clientCredentials.serverKey->data(),
                         clientCredentials.serverKey->size(),
                         reinterpret_cast<const unsigned char*>(authMessage.c_str()),
                         authMessage.size());

    std::string encodedServerSignature = hashToBase64(serverSignature);

    if (encodedServerSignature.size() != receivedServerSignature.size()) {
        return false;
    }

    return consttimeMemEqual(
        reinterpret_cast<const unsigned char*>(encodedServerSignature.c_str()),
        reinterpret_cast<const unsigned char*>(receivedServerSignature.c_str()),
        encodedServerSignature.size());
}

bool verifyClientProof(StringData clientProof, StringData storedKey, StringData authMessage) {
    // ClientSignature := HMAC(StoredKey, AuthMessage)
    SHA1Hash clientSignature =
        crypto::hmacSha1(reinterpret_cast<const unsigned char*>(storedKey.rawData()),
                         storedKey.size(),
                         reinterpret_cast<const unsigned char*>(authMessage.rawData()),
                         authMessage.size());

    // ClientKey := ClientSignature XOR ClientProof
    SHA1Hash clientKey;
    for (size_t i = 0; i < clientKey.size(); i++) {
        clientKey[i] = clientSignature[i] ^ clientProof.rawData()[i];
    }

    // StoredKey := H(ClientKey)
    SHA1Hash computedStoredKey = crypto::sha1(clientKey.data(), clientKey.size());

    return consttimeMemEqual(reinterpret_cast<const unsigned char*>(storedKey.rawData()),
                             computedStoredKey.data(),
                             computedStoredKey.size());
}

}  // namespace scram
}  // namespace mongo