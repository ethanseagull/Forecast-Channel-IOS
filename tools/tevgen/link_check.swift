// Links generated specialized pixel shaders (tools/tevgen/samples) with the
// ubershader's vertex shader into Metal render pipelines, as SDL3 GPU does
// at run time: catches interface mismatches (stage-in attributes, types,
// bindings) that compiling each file alone cannot. Run on the macOS CI:
//
//   xcrun swift tools/tevgen/link_check.swift <vs.metal> <ps.metal>...
//
// Skips (exit 0) when the runner has no Metal device.
import Foundation
import Metal

let args = CommandLine.arguments
guard args.count >= 3 else {
    print("usage: link_check.swift <vs.metal> <ps.metal>...")
    exit(2)
}
guard let device = MTLCreateSystemDefaultDevice() else {
    print("link_check: no Metal device on this machine; skipped")
    exit(0)
}
print("link_check: \(device.name)")

func function(_ path: String) throws -> MTLFunction {
    let source = try String(contentsOfFile: path, encoding: .utf8)
    let library = try device.makeLibrary(source: source, options: nil)
    guard let f = library.makeFunction(name: "main0") else {
        throw NSError(domain: "link_check", code: 1, userInfo: [NSLocalizedDescriptionKey: "no main0 in \(path)"])
    }
    return f
}

// gx_sdl.cpp's vertex layout (struct GXVtxOut): pos, clr0, clr1 as float4,
// tex0-7 as float3, 36 floats a vertex. SDL binds vertex buffer 0 at
// buffer(14).
let vertexDescriptor = MTLVertexDescriptor()
var offset = 0
for i in 0..<11 {
    let a = vertexDescriptor.attributes[i]!
    a.format = i < 3 ? .float4 : .float3
    a.offset = offset
    a.bufferIndex = 14
    offset += i < 3 ? 16 : 12
}
vertexDescriptor.layouts[14]!.stride = offset

var failures = 0
do {
    let vs = try function(args[1])
    for path in args[2...] {
        do {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = vs
            d.fragmentFunction = try function(path)
            d.vertexDescriptor = vertexDescriptor
            d.colorAttachments[0].pixelFormat = .rgba8Unorm
            d.depthAttachmentPixelFormat = .depth32Float
            _ = try device.makeRenderPipelineState(descriptor: d)
        } catch {
            print("link_check: \(path): \(error)")
            failures += 1
        }
    }
} catch {
    print("link_check: vertex shader \(args[1]): \(error)")
    exit(1)
}
print("link_check: \(args.count - 2 - failures) of \(args.count - 2) pipelines linked")
exit(failures == 0 ? 0 : 1)
