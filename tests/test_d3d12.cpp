#include "d3d12_cases.h"

using namespace d3d12_cases;

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "Expected fullscreen, pack, and unpack DXBC paths\n";
        return EXIT_FAILURE;
    }
    const std::vector<char> vertex = ReadBinary(argv[1]);
    const std::vector<char> pack = ReadBinary(argv[2]);
    const std::vector<char> unpack = ReadBinary(argv[3]);
    Check(!vertex.empty() && !pack.empty() && !unpack.empty(),
          "compiled DXBC fixtures are readable");
    if (failures != 0) return EXIT_FAILURE;

    RunAdapterCases(vertex, pack, unpack);
    if (failures != 0) {
        std::cerr << failures << " D3D12 smoke-test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Optimizer FPS SDK D3D12 GPU smoke test passed\n";
    return EXIT_SUCCESS;
}
