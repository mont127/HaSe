/*
 * CheeseBridge Phase 5 smoke payload.
 *
 * This is a deliberately tiny Windows Direct3D 11 program for the first
 * Wine + DXVK test.  It creates a Win32 window, creates a D3D11 swapchain,
 * clears the backbuffer with a changing color for a few seconds, presents,
 * and exits.  When run under Wine with DXVK installed, the call path should
 * become:
 *
 *   d3d11_smoke.exe -> Wine -> DXVK -> Vulkan -> CheeseBridge ICD -> host
 */

#define COBJMACROS

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

static LRESULT CALLBACK smoke_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wparam, LPARAM lparam) {
    (void)wparam;
    (void)lparam;
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int main(void) {
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = smoke_wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "CheeseBridgeD3D11SmokeWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "RegisterClassA failed: %lu\n", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "CheeseBridge D3D11 Smoke",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
                                NULL, NULL, inst, NULL);
    if (!hwnd) {
        fprintf(stderr, "CreateWindowExA failed: %lu\n", GetLastError());
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof sd);
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 360;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_10_0;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *swapchain = NULL;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        levels, (UINT)(sizeof levels / sizeof levels[0]),
        D3D11_SDK_VERSION, &sd, &swapchain, &device, &chosen, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n",
                (unsigned long)hr);
        return 2;
    }

    ID3D11Texture2D *backbuffer = NULL;
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D,
                                  (void **)&backbuffer);
    if (FAILED(hr)) {
        fprintf(stderr, "IDXGISwapChain_GetBuffer failed: 0x%08lx\n",
                (unsigned long)hr);
        return 3;
    }

    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)backbuffer, NULL, &rtv);
    ID3D11Texture2D_Release(backbuffer);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateRenderTargetView failed: 0x%08lx\n",
                (unsigned long)hr);
        return 4;
    }

    printf("D3D11 smoke running, feature level 0x%04x\n", (unsigned)chosen);

    int running = 1;
    for (int frame = 0; frame < 300 && running; ++frame) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        float t = (float)(frame % 180) / 179.0f;
        float color[4] = { 0.08f + 0.65f * t, 0.18f, 0.85f - 0.55f * t, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
        hr = IDXGISwapChain_Present(swapchain, 1, 0);
        if (FAILED(hr)) {
            fprintf(stderr, "Present failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }
        Sleep(16);
    }

    ID3D11RenderTargetView_Release(rtv);
    IDXGISwapChain_Release(swapchain);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(device);
    DestroyWindow(hwnd);
    printf("D3D11 smoke finished\n");
    return 0;
}
