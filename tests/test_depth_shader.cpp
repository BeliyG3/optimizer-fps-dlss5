// Build with the VS x64 tools; no game, vendor runtime, or hardware GPU required.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using Vec = std::array<float, 4>;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D operation failed"); }
static void Require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static ComPtr<ID3D11ShaderResourceView> Texture(ID3D11Device* device, const Vec& value)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{value.data(), sizeof(Vec), 0};
    ComPtr<ID3D11Texture2D> texture; Check(device->CreateTexture2D(&desc,&data,&texture));
    ComPtr<ID3D11ShaderResourceView> view;
    Check(device->CreateShaderResourceView(texture.Get(),nullptr,&view)); return view;
}
int main(int argc, char** argv)
{
    try {
        Require(argc == 2, "usage: test_depth_shader <compiled shader>");
        ComPtr<ID3DBlob> code;
        Check(D3DReadFileToBlob(std::filesystem::path(argv[1]).c_str(),&code));
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        context->CSSetShader(shader.Get(),nullptr,0);
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=6*sizeof(Vec); desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS; desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride=sizeof(Vec);
        ComPtr<ID3D11Buffer> output; Check(device->CreateBuffer(&desc,nullptr,&output));
        ComPtr<ID3D11UnorderedAccessView> uav; Check(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
        auto rawUav=uav.Get(); context->CSSetUnorderedAccessViews(0,1,&rawUav,nullptr);
        desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        desc.MiscFlags=0; desc.StructureByteStride=0;
        ComPtr<ID3D11Buffer> readback; Check(device->CreateBuffer(&desc,nullptr,&readback));
        desc={}; desc.ByteWidth=9*sizeof(Vec); desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> constants; Check(device->CreateBuffer(&desc,nullptr,&constants));
        auto rawConstants=constants.Get(); context->CSSetConstantBuffers(0,1,&rawConstants);
        const std::array<Vec,4> pairs{{{-2.3f,-84.f,-15.1659f,-15.1568f},
            {-0.2f,-0.2001f,200.f,200.01f},{0.2f,0.2001f,0.8f,0.7999f},
            {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),0,0}}};
        desc.ByteWidth=sizeof(pairs);
        D3D11_SUBRESOURCE_DATA pairData{pairs.data(),0,0};
        ComPtr<ID3D11Buffer> pairBuffer; Check(device->CreateBuffer(&desc,&pairData,&pairBuffer));
        auto rawPairs=pairBuffer.Get(); context->CSSetConstantBuffers(1,1,&rawPairs);
        D3D11_SAMPLER_DESC samplerDesc{}; samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler; Check(device->CreateSamplerState(&samplerDesc,&sampler));
        auto rawSampler=sampler.Get(); context->CSSetSamplers(0,1,&rawSampler);
        int cases=0;
        for(float depth : {-84.f,-15.1659f,-2.f,-1.f,-0.2f,0.2f,0.8f,15.f,200.f}) {
            for(int age=0; age<2; ++age) for(int mode=0; mode<6; ++mode) {
                // Known, unknown, disabled, and a newly uncovered surface.
                const float nan=std::numeric_limits<float>::quiet_NaN();
                Vec expectation{(mode==1 || mode==2) ? nan : depth,1,depth,mode==2 ? 1.f : 0.f};
                auto now=Texture(device.Get(),{depth,0,0,0});
                auto previous=Texture(device.Get(),{mode==3 ? depth*30 : depth,0,0,0});
                auto expect=Texture(device.Get(),expectation);
                auto chain=Texture(device.Get(),{mode==5 ? nan : 0.f,0,0,0});
                ID3D11ShaderResourceView* chainView=chain.Get(); context->CSSetShaderResources(4,1,&chainView);
                ID3D11ShaderResourceView* raw=now.Get(); context->CSSetShaderResources(5,1,&raw);
                raw=previous.Get(); context->CSSetShaderResources(6,1,&raw);
                raw=expect.Get(); context->CSSetShaderResources(11,1,&raw);
                auto historyLink=Texture(device.Get(),{0,0,depth,mode==4 ? depth*30 : depth});
                auto historyColour=Texture(device.Get(),{0,0,0,0});
                auto historyResidual=Texture(device.Get(),{1,1,1,0});
                raw=historyLink.Get(); context->CSSetShaderResources(18,1,&raw);
                raw=now.Get(); context->CSSetShaderResources(16,1,&raw);
                raw=historyColour.Get(); context->CSSetShaderResources(14,1,&raw);
                raw=historyResidual.Get(); context->CSSetShaderResources(12,1,&raw);
                std::array<Vec,9> cb{};
                cb[0]={1,1,1,1}; cb[1]=cb[2]=cb[3]={0,0,1,1}; cb[4]={1,1,1,1};
                cb[5]={mode==2 ? 0.f : 1.f,float(age),0,0.02f}; cb[6]={0,1,0,0};
                context->UpdateSubresource(constants.Get(),0,nullptr,cb.data(),0,0);
                context->Dispatch(1,1,1); context->CopyResource(readback.Get(),output.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
                const auto* result=static_cast<const Vec*>(mapped.pData);
                Require(result[0][0]>0.97f,"Different negative surfaces accepted");
                Require(result[0][1]<0.001f && result[0][2]<0.001f && result[0][3]<0.001f,"Same signed linear surface rejected");
                Require(std::abs(result[1][0]-result[1][1])<1e-6f,"Forward/reverse metric asymmetry");
                Require(result[1][2]==1e9f && result[1][3]==1e9f,"Non-finite metric must reject");
                if((mode==1 && age==1) || mode==3) Require(std::isnan(result[2][0]),"Unknown expectation must be NaN");
                else Require(result[2][0]==depth,"Finite expectation changed");
                Require(result[2][3]==(mode==2 ? 1.f : 0.f),"Disabled flag changed");
                if(mode==1) Require(std::isnan(result[3][0]) && result[3][1]==1e9f,"Unknown expectation must reject");
                else Require(result[3][0]==depth,"Expected depth sign or off fallback changed");
                if(mode!=3) Require(result[3][2]==0,"Real depth rejected by around test");
                if(mode!=2) Require(result[4][2]==depth,"History discarded signed geometric depth");
                if(mode!=4) Require(result[5][0]==1 && result[5][3]==0.85f,"Older-pass recovery rejected signed depth");
                else Require(result[5][3]==0,"Coarse link borrowed the other surface despite a matching guide");
                if(mode!=2) Require(result[4][3]==depth,"History source depth was not stored with its link");
                if(mode==5) Require(std::isnan(result[4][0]),"Invalid history chain was fabricated as static motion");
                context->Unmap(readback.Get(),0); ++cases;
            }
        }
        std::printf("PASS: %d production-shader depth/expectation/history cases on WARP\n",cases);
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
