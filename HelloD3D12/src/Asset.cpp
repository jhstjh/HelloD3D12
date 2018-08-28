#include "stdafx.h"

#include <cassert>
#include "Asset.h"

#pragma warning(disable:4996)

#ifdef _ANDROID

#include <android/asset_manager.h>

static AAssetManager* gAssetManager = nullptr;

void Asset::setAssetManager(void* assetManager)
{
    gAssetManager = reinterpret_cast<AAssetManager*>(assetManager);
}

struct Asset::Impl
{
    Impl(std::string filename, uint32_t /*openMode*/)
    {
        assert(gAssetManager);
        mAsset = AAssetManager_open(gAssetManager, filename.c_str(), AASSET_MODE_UNKNOWN);
    }

    ~Impl()
    {
        if (mAsset)
        {
            close();
        }
    }

    uint32_t getLength() 
    {
        assert(mAsset);
        return AAsset_getLength(mAsset);
    }

    void read(uint8_t* data, uint32_t size)
    {
        AAsset_read(mAsset, data, size);
    }

    void close() 
    {
        assert(mAsset);
        AAsset_close(mAsset);
        mAsset = nullptr;
    }

    AAsset* mAsset{ nullptr };
};

#else

#include <cstdio>
#include <direct.h>

void Asset::setAssetManager(void* /*assetManager*/)
{

}

struct Asset::Impl
{
    Impl(std::string filename, uint32_t /*openMode*/)
    {
        char cwd[256];
        _getcwd(cwd, 256);

        filename = "assets/" + filename;
        mAsset = fopen(filename.c_str(), "rb");
        fseek(mAsset, 0L, SEEK_END);
        mSize = ftell(mAsset);
        fseek(mAsset, 0L, SEEK_SET);
    }

    ~Impl()
    {
        if (mAsset)
        {
            close();
        }
    }

    uint32_t getLength()
    {
        return mSize;
    }

    void read(uint8_t* data, uint32_t size)
    {
        fread(data, size, 1, mAsset);
    }

    void close()
    {
        assert(mAsset);
        fclose(mAsset);
        mAsset = nullptr;
    }

    FILE* mAsset{ nullptr };
    uint32_t mSize{ 0 };
};


#endif

Asset::Asset(std::string filename, uint32_t openMode)
{
    mImpl = std::make_unique<Impl>(filename, openMode);
}

Asset::~Asset()
{
    mImpl = nullptr;
}

uint32_t Asset::getLength()
{
    return mImpl->getLength();
}

void Asset::read(void* data, uint32_t size)
{
    return mImpl->read(reinterpret_cast<uint8_t*>(data), size);
}

void Asset::close()
{
    mImpl->close();
}
