#pragma once
#include <stdint.h>

namespace HDX
{

class Renderer
{
public:
    static Renderer* create();
    static Renderer& getInstance();
    static bool isCreated();

    virtual void release() = 0;
    virtual ~Renderer() {};

    virtual bool init(HWND hWnd, uint32_t width, uint32_t height) = 0;
    virtual void onRender() = 0;
};

}
