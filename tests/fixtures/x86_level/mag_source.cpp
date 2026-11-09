extern "C" float fixture_runtime_madd(float, float, float);
extern "C" float fixture_lifted_ppu(float, float, float);
extern "C" float fixture_lifted_spu(float, float, float);
int main() { return fixture_runtime_madd(1, 1, 1) + fixture_lifted_ppu(1, 1, 1) + fixture_lifted_spu(1, 1, 1) == 6 ? 0 : 1; }
