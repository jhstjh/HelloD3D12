#pragma once
#include <memory>
#include <string>

class Asset
{
public:
    static void setAssetManager(void* assetManager);

    Asset(std::string filename, uint32_t openMode);
    ~Asset();
    uint32_t getLength();
    void read(void* data, uint32_t size);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

