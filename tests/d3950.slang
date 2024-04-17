//TEST:SIMPLE(): -target spirv -emit-spirv-directly -o /tmp/test.spv
// -force-glsl-scalar-layout -fvk-use-entrypoint-name are just renames
//TEST(compute, vulkan):COMPARE_COMPUTE(filecheck-buffer=BUF):-vk -compute -entry computeMain -emit-spirv-directly -output-using-type

import glsl;

//TEST_INPUT:tmp(data=[0], stride=4):out,name outputBuffer
RWStructuredBuffer<uint> outputBuffer;

struct Constants {
  uint *device_memory;
}
//TEST_INPUT:tmp2():out,name push_constants
[[vk::push_constant]] Constants push_constants;

groupshared uint shared_memory[1];

[shader("compute")] [numthreads(1, 1, 1)] void computeMain(void) {
  shared_memory[0] = 0xffffffff;

  atomicMin(shared_memory[0], 21);

  //BUF: 21
  push_constants.device_memory[0] = shared_memory[0];
}
