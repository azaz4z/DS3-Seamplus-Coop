#include "../src/render/ally_outline.h"
#include "../src/render/native_draw_packet.h"
#include "../src/render/ally_pass_visibility.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <thread>

using namespace ds3sc::render;
using Microsoft::WRL::ComPtr;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void HR(HRESULT hr) { Check(SUCCEEDED(hr), "D3D11 operation failed"); }
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* ctx, UINT count, UINT start) { ctx->Draw(count, start); }

int main() {
    HWND window = nullptr;
    try {
        // The producer has gone out of scope; only packet identity survives.
        std::array<std::uintptr_t, 0x130 / 8> packet{};
        packet[0x48 / 8] = 0x12345000 + 0xd0;
        packet[0x110 / 8] = 0x98765000;
        Check(PacketDrawEntity(reinterpret_cast<std::uintptr_t>(packet.data())) == 0x12345000,
              "packet interface offset is wrong");
        packet[0x48 / 8] = 0;
        packet[0x8 / 8] = 0x12345000;
        Check(!PacketDrawEntity(reinterpret_cast<std::uintptr_t>(packet.data())), "descriptor mistaken for actor");
        Check(!PacketDrawEntity(0), "null packet accepted");

        // A normally batched character must retain individual packet identity
        // for every submission, without touching its materials/bones/LOD flags.
        alignas(16) std::array<std::byte, 0xc0> options{};
        NativeSubmissionOptions individual{};
        Check(individual.size() >= 0xb8, "submission drops native bone/material pointers beyond the header");
        for (std::size_t i = 0; i < options.size(); ++i) options[i] = static_cast<std::byte>(i + 1);
        for (int frame = 0; frame < 120; ++frame) {
            const auto before = options;
            Check(IndividualAllySubmission(options.data(), individual), "individual submission failed");
            Check(individual[0xe] == std::byte{0}, "ally still enters merged render queue");
            Check(options == before, "engine submission options were mutated");
            individual[0xe] = options[0xe];
            Check(individual == options, "unrelated submission options changed");
        }
        Check(!IndividualAllySubmission(nullptr, individual), "null submission accepted");

        // Reproduce the native 5..8 early exit across successive hidden frames.
        // A visible model, other passes and all adjacent native state stay intact.
        std::array<std::uint8_t, 0xc60> entity{};
        entity.fill(0x5a);
        for (std::uint32_t pass = 0; pass < 70; ++pass) {
            const auto before = entity;
            {
                ScopedAllyPassVisibility visibility(reinterpret_cast<std::uintptr_t>(entity.data()), pass);
                Check(visibility.Changed() == (pass >= 5 && pass <= 8), "wrong native pass override");
                Check(entity[0xa5d] == (visibility.Changed() ? 0 : before[0xa5d]), "occluded model remains culled");
                auto onlyFlag = entity;
                onlyFlag[0xa5d] = before[0xa5d];
                Check(onlyFlag == before, "unrelated model state was overwritten");
            }
            Check(entity == before, "native visibility flag was not restored");
        }
        entity[0xa5d] = 0;
        ScopedAllyPassVisibility visible(reinterpret_cast<std::uintptr_t>(entity.data()), 5);
        Check(!visible.Changed(), "visible actor state changed");

        // Verify ScopedAllyModelOverride and MakeAllyPersistentlyVisible
        entity.fill(0x00);
        entity[0xc3c] = 0x00;
        entity[0xa5d] = 0x01;
        *reinterpret_cast<float*>(&entity[0x170]) = 0.0f;
        MakeAllyPersistentlyVisible(reinterpret_cast<std::uintptr_t>(entity.data()));
        Check((entity[0xc3c] & 1) == 1, "MakeAllyPersistentlyVisible failed to set CPU visibility");
        Check(entity[0xa5d] == 0, "MakeAllyPersistentlyVisible failed to clear wall occlusion");
        Check(*reinterpret_cast<float*>(&entity[0x170]) >= 1.0f, "MakeAllyPersistentlyVisible failed to restore fade");
        bool allDrawGroupsSet = true;
        for (std::size_t i = 0x88; i < 0x88 + 32; ++i) {
            if (entity[i] != 0xFF) { allDrawGroupsSet = false; break; }
        }
        Check(allDrawGroupsSet, "MakeAllyPersistentlyVisible failed to set Draw Groups to 0xFF");

        // Verify ScopedAllyModelOverride RAII restoration
        entity.fill(0x33);
        entity[0xc3c] = 0x00;
        entity[0xa5d] = 0x01;
        *reinterpret_cast<float*>(&entity[0x170]) = 0.0f;
        const auto modelBefore = entity;
        {
            ScopedAllyModelOverride modelOverride(reinterpret_cast<std::uintptr_t>(entity.data()), 5);
            Check(modelOverride.Changed(), "ScopedAllyModelOverride did not report change");
            Check((entity[0xc3c] & 1) == 1, "ScopedAllyModelOverride failed to set CPU vis");
            Check(entity[0xa5d] == 0, "ScopedAllyModelOverride failed to clear wall occlusion");
            Check(*reinterpret_cast<float*>(&entity[0x170]) >= 1.0f, "ScopedAllyModelOverride failed to restore fade");
        }
        Check(entity == modelBefore, "ScopedAllyModelOverride failed to restore original state");

        window = CreateWindowExW(0, L"STATIC", L"Outline WARP", WS_OVERLAPPEDWINDOW,
            0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "window creation failed");
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = sd.BufferDesc.Height = 128;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferCount = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.OutputWindow = window;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGISwapChain> swap;
        HR(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
            &sd, &swap, &device, nullptr, &ctx));
        ComPtr<ID3D11InfoQueue> diagnostics; HR(device.As(&diagnostics));
        ComPtr<ID3D11Texture2D> back, depth, readback;
        HR(swap->GetBuffer(0, IID_PPV_ARGS(&back)));
        ComPtr<ID3D11RenderTargetView> target;
        HR(device->CreateRenderTargetView(back.Get(), nullptr, &target));
        D3D11_TEXTURE2D_DESC td{}; back->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&td, nullptr, &readback));
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        HR(device->CreateTexture2D(&td, nullptr, &depth));
        ComPtr<ID3D11DepthStencilView> dsv; HR(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
        const char shader[] = R"(
            float4 VS(uint id:SV_VertexID):SV_Position {
                float2 p[6] = {float2(-.5,-.5),float2(-.5,.5),float2(.5,.5),
                               float2(-.5,-.5),float2(.5,.5),float2(.5,-.5)};
                return float4(p[id],.7,1);
            }
            float4 VSSecond(uint id:SV_VertexID):SV_Position { return VS(id + 3); }
            float4 VSLocalFront(uint id:SV_VertexID):SV_Position {
                float4 p = VS(id); p.x -= .5; p.z = .3; return p;
            }
            float4 VSLocalBack(uint id:SV_VertexID):SV_Position {
                float4 p = VSLocalFront(id); p.z = .9; return p;
            }
            float4 PS():SV_Target { return float4(0,1,0,1); }
        )";
        ComPtr<ID3DBlob> code;
        HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &code, nullptr));
        ComPtr<ID3D11VertexShader> vs;
        HR(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
        code.Reset();
        HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &code, nullptr));
        ComPtr<ID3D11PixelShader> ps;
        HR(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
        auto& live = LiveAllyOutline::Instance();
        HR(live.Present(swap.Get(), false)); // Acquire actual swapchain and render thread.
        D3D11_VIEWPORT vp{0,0,128,128,0,1};
        auto setup = [&](float sceneDepth, D3D11_COMPARISON_FUNC comparison) {
            ctx->ClearState();
            auto* rtv = target.Get(); ctx->OMSetRenderTargets(1, &rtv, dsv.Get());
            const float black[4]{0,0,0,.37f}; ctx->ClearRenderTargetView(target.Get(), black);
            live.BeforeDepthClear(ctx.Get(), dsv.Get(), D3D11_CLEAR_DEPTH, sceneDepth);
            ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, sceneDepth, 0);
            D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE;
            ds.DepthFunc = comparison; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            ComPtr<ID3D11DepthStencilState> state; HR(device->CreateDepthStencilState(&ds, &state));
            ctx->OMSetDepthStencilState(state.Get(), 19);
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
        };
        auto capture = [&]() {
            ReplayDrawParams params{}; params.type = DrawCallType::Draw; params.vertexCount = 6;
            return live.Capture(ctx.Get(), params, reinterpret_cast<void*>(&Draw));
        };
        auto pixel = [&](UINT x, UINT y, UINT channel = 0) {
            ctx->CopyResource(readback.Get(), back.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{}; HR(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            auto value = static_cast<unsigned char*>(mapped.pData)[y * mapped.RowPitch + x * 4 + channel];
            ctx->Unmap(readback.Get(), 0); return value;
        };
        ComPtr<ID3D11VertexShader> localFront, localBack;
        auto makeVS = [&](const char* entry, ComPtr<ID3D11VertexShader>& output) {
            code.Reset();
            HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, entry, "vs_5_0", 0, 0, &code, nullptr));
            HR(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &output));
        };
        makeVS("VSLocalFront", localFront); makeVS("VSLocalBack", localBack);
        auto captureLocal = [&](ID3D11VertexShader* localShader) {
            ctx->VSSetShader(localShader, nullptr, 0);
            ReplayDrawParams params{}; params.type = DrawCallType::Draw; params.vertexCount = 6;
            const bool accepted = live.Capture(ctx.Get(), params, reinterpret_cast<void*>(&Draw), true);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            return accepted;
        };
        // A player covers only the left half of the ally. Protect both the edge
        // and fill, retain the other half, and work with either capture order.
        for (int reverse = 0; reverse < 2; ++reverse) for (int order = 0; order < 2; ++order) {
            setup(reverse ? .9f : .3f, reverse ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL);
            auto* playerVS = reverse ? localBack.Get() : localFront.Get();
            if (order) Check(capture(), "ally-first capture failed");
            Check(captureLocal(playerVS), "local-player capture failed");
            if (!order) Check(capture(), "player-first ally capture failed");
            HR(live.Present(swap.Get(), true, false, 2, false, true));
            Check(pixel(32,64) == 0 && pixel(48,64) == 0, "ally overlay painted over local player");
            Check(pixel(95,64) > 100 && pixel(80,64) > 50, "player mask erased unrelated ally pixels");
            Check(pixel(32,64,3) == 94, "player exclusion changed destination alpha");
            Check(live.CapturesThisFrame() == 1, "player was counted as an ally");
        }
        setup(.1f, D3D11_COMPARISON_LESS_EQUAL);
        Check(captureLocal(localFront.Get()) && capture(), "wall-over-both capture failed");
        HR(live.Present(swap.Get(), true, false, 2, false, true));
        Check(pixel(32,64) > 100 && pixel(48,64) > 50, "hidden player erased ally through wall");
        setup(.7f, D3D11_COMPARISON_LESS_EQUAL);
        Check(captureLocal(localBack.Get()) && capture(), "ally-in-front capture failed");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100, "player behind ally incorrectly blocked outline");
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(captureLocal(localFront.Get()), "player-only capture failed");
        Check(live.Present(swap.Get(), true) == S_FALSE, "player alone produced an ally outline");
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(capture(), "capture after player-only frame failed");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100, "old player mask leaked into next frame");
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(capture(), "occluded submesh not captured");
        ComPtr<ID3D11PixelShader> restoredPS; ctx->PSGetShader(&restoredPS, nullptr, nullptr);
        Check(restoredPS == ps, "pixel shader not restored");
        ComPtr<ID3D11RenderTargetView> restoredRT; ComPtr<ID3D11DepthStencilView> restoredDS;
        ctx->OMGetRenderTargets(1, &restoredRT, &restoredDS);
        Check(restoredRT == target && restoredDS == dsv, "output merger not restored");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100 && pixel(64,64) == 0, "occluded contour pixels wrong");
        Check(pixel(32,64,3) == 94, "destination alpha changed");
        setup(1, D3D11_COMPARISON_LESS_EQUAL);
        Check(capture(), "visible submesh not captured");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100, "visible contour missing");
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(capture(), "capture before scene reuse failed");
        live.BeforeDepthClear(ctx.Get(), dsv.Get(), D3D11_CLEAR_DEPTH, 1);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        Check(!capture(), "second camera mixed with frozen scene");
        HR(live.Present(swap.Get(), true, false, 2, false));
        Check(pixel(32,64) > 100, "depth was copied after scene reuse");
        setup(.9f, D3D11_COMPARISON_GREATER_EQUAL);
        Check(capture(), "reversed depth capture failed");
        HR(live.Present(swap.Get(), true, false, 2, false));
        Check(pixel(32,64) > 100, "reversed depth contour missing");
        setup(0, D3D11_COMPARISON_EQUAL);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, .9f, 0);
        Check(capture(), "EQUAL did not retain reversed clear convention");
        HR(live.Present(swap.Get(), true, false, 2, false));
        Check(pixel(32,64) > 100, "EQUAL inferred wrong depth convention");

        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        ComPtr<ID3D11Predicate> predicate; HR(device->CreatePredicate(&qd, &predicate));
        ctx->Begin(predicate.Get()); ctx->End(predicate.Get());
        BOOL samples = TRUE;
        HRESULT query = S_FALSE;
        const auto deadline = GetTickCount64() + 5000;
        while ((query = ctx->GetData(predicate.Get(), &samples, sizeof(samples), 0)) == S_FALSE && GetTickCount64() < deadline)
            SwitchToThread();
        Check(query == S_OK && !samples, "empty occlusion query did not complete");
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        ctx->SetPredication(predicate.Get(), FALSE);
        ctx->Draw(6,0);
        ctx->SetPredication(nullptr, FALSE);
        Check(pixel(64,64,1) == 0, "control draw was not actually suppressed by predication");
        ctx->SetPredication(predicate.Get(), FALSE);
        Check(capture(), "predicated capture failed");
        ComPtr<ID3D11Predicate> restored; BOOL value = FALSE; ctx->GetPredication(&restored, &value);
        Check(restored == predicate && !value, "predication not restored");
        HR(live.Present(swap.Get(), true, true));
        restored.Reset(); ctx->GetPredication(&restored, &value);
        Check(restored == predicate && !value, "Present predication not restored");
        ctx->SetPredication(nullptr, FALSE);
        Check(pixel(64,64) > 100, "occlusion predicate suppressed independent mask");
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        D3D11_VIEWPORT shadow{0,0,64,64,0,1}; ctx->RSSetViewports(1, &shadow);
        Check(!capture(), "shadow viewport replaced main mask");
        ctx->RSSetViewports(1, &vp);
        Check(capture(), "shadow rejection broke main capture");
        HR(live.Present(swap.Get(), false)); Check(pixel(32,64) == 0, "disable ignored");
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(live.Present(swap.Get(), true) == S_FALSE, "stale silhouette reused");
        Check(pixel(32,64) == 0, "stale pixels on empty frame");
        // Serialized immediate-context handoff: Present and native rendering
        // need not execute on the same OS thread.
        bool workerCaptured = false;
        std::thread worker([&] {
            setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
            workerCaptured = capture();
        });
        worker.join();
        Check(workerCaptured, "serialized render-thread handoff rejected");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100, "worker-thread capture missing at Present");
        // Work is recorded on separate contexts, then executed in reverse order.
        // An unexecuted list must not publish a silhouette at Present.
        code.Reset();
        HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VSSecond", "vs_5_0", 0, 0, &code, nullptr));
        ComPtr<ID3D11VertexShader> secondVS;
        HR(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &secondVS));
        auto immediate = ctx;
        ComPtr<ID3D11DeviceContext> deferredA, deferredB;
        HR(device->CreateDeferredContext(0, &deferredA));
        HR(device->CreateDeferredContext(0, &deferredB));
        auto recordActor = [&](ID3D11DeviceContext* deferred, bool local) {
            ctx = deferred;
            setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
            Check(local ? captureLocal(localFront.Get()) : capture(), "deferred actor capture failed");
            ComPtr<ID3D11CommandList> list;
            HR(ctx->FinishCommandList(FALSE, &list)); live.Finish(ctx.Get(), list.Get());
            ctx = immediate;
            return list;
        };
        auto playerList = recordActor(deferredA.Get(), true);
        auto allyList = recordActor(deferredB.Get(), false);
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(capture(), "immediate ally without executed player failed");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100, "unexecuted player list suppressed the ally");
        for (int order = 0; order < 2; ++order) {
            auto* first = order ? playerList.Get() : allyList.Get();
            auto* second = order ? allyList.Get() : playerList.Get();
            ctx->ExecuteCommandList(first, TRUE); live.Executed(first);
            ctx->ExecuteCommandList(second, TRUE); live.Executed(second);
            HR(live.Present(swap.Get(), true, false, 2, false, true));
            Check(pixel(32,64) == 0 && pixel(48,64) == 0 && pixel(95,64) > 100,
                "deferred/reused player exclusion lost depth or execution order");
        }
        playerList.Reset(); allyList.Reset();
        auto record = [&](ID3D11DeviceContext* deferred, UINT start, BOOL restore) {
            ctx = deferred;
            setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
            ReplayDrawParams params{}; params.type = DrawCallType::Draw;
            params.vertexCount = 3; params.startVertexLocation = 0;
            if (start) ctx->VSSetShader(secondVS.Get(), nullptr, 0);
            Check(live.Capture(ctx.Get(), params, reinterpret_cast<void*>(&Draw)), "deferred capture rejected");
            ComPtr<ID3D11CommandList> list;
            HR(ctx->FinishCommandList(restore, &list)); live.Finish(ctx.Get(), list.Get());
            ctx = immediate;
            return list;
        };
        auto listA = record(deferredA.Get(), 0, FALSE);
        auto listB = record(deferredB.Get(), 3, TRUE);
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(live.Present(swap.Get(), true) == S_FALSE, "recording counted as execution");
        ctx->ExecuteCommandList(listB.Get(), TRUE); Check(live.Executed(listB.Get()), "list B metadata lost");
        ctx->ExecuteCommandList(listA.Get(), FALSE); Check(live.Executed(listA.Get()), "list A metadata lost");
        // Scene reuse in a separate clear-only command list must freeze depth.
        live.BeforeDepthClear(deferredB.Get(), dsv.Get(), D3D11_CLEAR_DEPTH, 1);
        deferredB->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        ComPtr<ID3D11CommandList> clearList;
        HR(deferredB->FinishCommandList(FALSE, &clearList)); live.Finish(deferredB.Get(), clearList.Get());
        ctx->ExecuteCommandList(clearList.Get(), TRUE); live.Executed(clearList.Get());
        HR(live.Present(swap.Get(), true, false, 2, false));
        Check(pixel(32,48) > 100 && pixel(95,80) > 100, "deferred lists erased each other or lost scene depth");
        // Re-execution has independent frame accounting, with no mutable CPU buffer replay.
        ctx->ExecuteCommandList(listA.Get(), TRUE); Check(live.Executed(listA.Get()), "reusable list rejected");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,48) > 100, "re-executed list missing");
        // A new level can replace scene depth without resizing the swapchain.
        dsv.Reset(); depth.Reset();
        HR(device->CreateTexture2D(&td, nullptr, &depth));
        HR(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
        setup(.3f, D3D11_COMPARISON_LESS_EQUAL);
        Check(capture(), "replacement scene depth stayed locked to old resource");
        HR(live.Present(swap.Get(), true));
        Check(pixel(32,64) > 100, "replacement scene depth lost outline");
        live.Reset(); HR(live.Present(swap.Get(), false));
        Check(!live.Executed(listB.Get()), "old resource generation accepted after reset");
        listA.Reset(); listB.Reset(); clearList.Reset(); deferredA.Reset(); deferredB.Reset();
        live.Reset(); ctx->ClearState(); ctx->Flush();
        for (UINT64 i = 0; i < diagnostics->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; HR(diagnostics->GetMessage(i, nullptr, &size));
            std::vector<unsigned char> storage(size);
            auto* msg = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); HR(diagnostics->GetMessage(i, msg, &size));
            if (msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "%s\n", msg->pDescription);
                throw std::runtime_error("D3D11 debug-layer warning/error");
            }
        }
        DestroyWindow(window);
        std::puts("PASS: live WARP capture/composition, visible/hidden/reversed/EQUAL, depth reuse,");
        std::puts("      GPU predication, state/alpha restoration, shadow rejection, stale frames, packet identity.");
        std::puts("PASS: deferred list ordering, unexecuted/reusable lists, depth-clear list, resource generations.");
        std::puts("PASS: local-player edge/fill exclusion, front/behind/wall, reverse Z, capture order and stale masks.");
        std::puts("PASS: D3D11 debug layer clean. Synthetic geometry; not a DS3 acceptance test.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        LiveAllyOutline::Instance().Reset(); if (window) DestroyWindow(window); return 1;
    }
}
