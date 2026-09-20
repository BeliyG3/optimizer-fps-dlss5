#include "pipeline.h"
#include <fstream>
std::vector<char> Shader(const char *name)
{
    auto path=ExecutableDirectory()/"shaders"/"bin"/name;
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("Missing shader: "+path.string()+"; run build.cmd");
    auto size=file.tellg(); if(size<=0) throw std::runtime_error("Empty shader: "+path.string());
    std::vector<char> bytes(static_cast<size_t>(size)); file.seekg(0); file.read(bytes.data(),size);
    if(!file) throw std::runtime_error("Cannot read shader: "+path.string()); return bytes;
}
ComPtr<ID3D12RootSignature> Root(Device &d, const D3D12_ROOT_SIGNATURE_DESC &desc)
{
    ComPtr<ID3DBlob> blob,error;
    HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
    if(FAILED(hr) && error) std::fprintf(stderr,"%s\n",static_cast<const char *>(error->GetBufferPointer()));
    Check(hr,"Serialize root signature"); ComPtr<ID3D12RootSignature> root;
    Check(d.gpu->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create root signature"); return root;
}
