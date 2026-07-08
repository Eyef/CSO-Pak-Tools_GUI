#pragma once

#include <array>
#include <cstdint>

constexpr const uint32_t SNOW_BLOCKS_SIZE = sizeof(uint32_t);
constexpr const uint32_t SNOW_NUM_BLOCKS = 16;

class SnowCipher
{
private:
    enum class BufferMode
    {
        Decrypt,
        Encrypt,
    };

    // State and keystream buffers for the decompiled CSO stream cipher.
    // m_Buffer stores generated keystream words, not encrypted file data.
    std::array<uint32_t, 20> m_State = {};
    std::array<uint32_t, SNOW_NUM_BLOCKS> m_Buffer = {};
    uint32_t m_BlocksAvailable = 0;

public:
    void SetKey(const uint8_t* key);

    template <typename T>
    inline void DecryptBuffer(T* outBuffer, const uint8_t* inBuffer,
                              uint32_t dataSize)
    {
        this->DecryptBufferImpl(reinterpret_cast<uint8_t*>(outBuffer),
                                inBuffer, dataSize);
    }

    // Encryption is the inverse of DecryptBufferImpl: same keystream,
    // add instead of subtract.
    template <typename T>
    inline void EncryptBuffer(T* outBuffer, const uint8_t* inBuffer,
                              uint32_t dataSize)
    {
        this->EncryptBufferImpl(reinterpret_cast<uint8_t*>(outBuffer),
                                inBuffer, dataSize);
    }

private:
    void InitializeStateFromKey(const uint8_t* key);
    void ProcessBufferImpl(uint8_t* outBuffer, const uint8_t* inBuffer,
                           uint32_t dataSize, BufferMode mode);
    void DecryptBufferImpl(uint8_t* outBuffer, const uint8_t* inBuffer,
                           uint32_t dataSize);
    void EncryptBufferImpl(uint8_t* outBuffer, const uint8_t* inBuffer,
                           uint32_t dataSize);
    void GenerateKeyStreamBlock();
};
