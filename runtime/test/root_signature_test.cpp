// Portable tests for runtime/src/root_signature.{h,cpp} (no windows.h).
//
// Own main + PASS/FAIL lines like the other runtime tests; returns nonzero
// on any failure so CTest gates on it. Everything here is hand-built: no
// game data, no DXBC, no serializer needed.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "root_signature.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

rt::rootsig::RootParameter consts(uint32_t n) {
    rt::rootsig::RootParameter p;
    p.type = rt::rootsig::ParamType::Constants;
    p.num_values = n;
    return p;
}

rt::rootsig::RootParameter view() {
    rt::rootsig::RootParameter p;
    p.type = rt::rootsig::ParamType::Descriptor;
    return p;
}

rt::rootsig::RootParameter table() {
    rt::rootsig::RootParameter p;
    p.type = rt::rootsig::ParamType::Table;
    p.ranges.push_back({0, 8});
    return p;
}

rt::rootsig::StaticSampler valid_non_normalized_sampler() {
    rt::rootsig::StaticSampler s;
    s.filter = 0x14;  // min/mag linear, mip point, standard reduction, no aniso
    s.address_u = rt::rootsig::kAddressModeClamp;
    s.address_v = rt::rootsig::kAddressModeClamp;
    s.address_w = rt::rootsig::kAddressModeWrap;  // AddressW is free
    s.border_color = rt::rootsig::kStaticBorderColorTransparentBlack;
    s.min_lod = 0.0f;
    s.max_lod = 0.0f;
    s.mip_lod_bias = 0.0f;
    s.flags = rt::rootsig::kSamplerFlagNonNormalizedCoordinates;
    return s;
}

rt::rootsig::StaticSampler valid_uint_border_sampler() {
    rt::rootsig::StaticSampler s;
    s.border_color = rt::rootsig::kStaticBorderColorOpaqueWhiteUint;
    s.flags = rt::rootsig::kSamplerFlagUintBorderColor;
    return s;
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

// Hand-built RTS0 payload in the offset-table layout the maintainer
// described: header, parameter table, then payloads at their offsets.
std::vector<uint8_t> build_payload() {
    std::vector<uint8_t> p;
    put_u32(p, 2);  // version 1.1
    put_u32(p, 2);  // 2 params
    put_u32(p, 24);  // params table offset
    put_u32(p, 0);  // 0 samplers
    put_u32(p, 0);  // samplers offset
    put_u32(p, 0x03000000u);  // Xbox flags
    // entry 0: constants at 48; entry 1: CBV descriptor at 60.
    put_u32(p, 1);
    put_u32(p, 0);
    put_u32(p, 48);
    put_u32(p, 2);
    put_u32(p, 4);
    put_u32(p, 60);
    put_u32(p, 7);  // register (ignored by packer)
    put_u32(p, 0);  // space
    put_u32(p, 3);  // 3 values
    put_u32(p, 0);  // register
    put_u32(p, 0);  // space
    put_u32(p, 0);  // 1.1 descriptor flags
    return p;
}

}  // namespace

int main() {
    using namespace rt::rootsig;

    // Mixed signature: 4 constants + CBV + table + 1 static sampler.
    {
        RootSignature sig;
        sig.params = {consts(4), view(), table()};
        StaticSampler s;
        s.shader_register = 3;
        sig.static_samplers = {s};
        const PackResult r = pack(sig);
        check(r.ok, "mixed signature packs");
        check(r.dword_count == 8, "mixed signature uses 8 dwords");
        check(r.d3d12_cost == 4 + 2 + 1, "mixed signature costs 7");
        check(r.provenance.size() == 8 && r.provenance[0].param_index == 0 &&
                  r.provenance[0].sub_index == 0 && r.provenance[4].param_index == 1 &&
                  r.provenance[4].sub_index == 0 && r.provenance[5].sub_index == 1 &&
                  r.provenance[6].param_index == 2,
              "provenance tracks param and sub-dword");
    }

    // Exactly 32 dwords still fits.
    {
        RootSignature sig;
        for (int i = 0; i < 16; ++i) sig.params.push_back(view());
        const PackResult r = pack(sig);
        check(r.ok && r.dword_count == 32, "16 views fill 32 dwords");
    }

    // 34 dwords: spill is refused loudly, never truncated.
    {
        RootSignature sig;
        for (int i = 0; i < 17; ++i) sig.params.push_back(view());
        const PackResult r = pack(sig);
        check(!r.ok && r.error.find("spill") != std::string::npos,
              "17 views refused as spill");
    }

    // 65 constants exceed the 64-dword D3D12 cost limit.
    {
        RootSignature sig;
        sig.params = {consts(65)};
        const PackResult r = pack(sig);
        check(!r.ok && r.error.find("64-dword") != std::string::npos,
              "65 constants refused by cost limit");
    }
    // A count that wraps a 32-bit sum (1 + 0xFFFFFFFF) is still refused, not packed past dwords[32].
    {
        rt::rootsig::RootSignature sig;
        sig.params = {consts(1), consts(0xFFFFFFFFu)};
        const auto r = rt::rootsig::pack(sig);
        check(!r.ok && r.error.find("64-dword") != std::string::npos,
              "a constant count that wraps 32 bits is refused by the cost limit");
    }

    // Canonical codec round-trips byte-exactly, sampler included.
    {
        RootSignature sig;
        sig.version = Version::V1_0;
        sig.params = {consts(2), table()};
        sig.flags = 0x03000000u;  // Xbox high byte survives the model
        StaticSampler s;
        s.filter = 21;
        s.mip_lod_bias = 0.5f;
        sig.static_samplers = {s};
        const std::vector<uint8_t> bytes = encode_canonical(sig);
        const DecodeResult d = decode_canonical(bytes.data(), bytes.size());
        check(d.ok, "canonical round-trip decodes");
        check(d.sig.params.size() == 2 && d.sig.params[0].num_values == 2 &&
                  d.sig.params[1].ranges.size() == 1 &&
                  d.sig.static_samplers.size() == 1 && d.sig.flags == 0x03000000u &&
                  d.sig.static_samplers[0].filter == 21,
              "canonical round-trip preserves model");
    }

    // Canonical decoder controls: bad magic, truncation, trailing bytes.
    {
        const std::vector<uint8_t> bad = {0, 1, 2, 3};
        check(!decode_canonical(bad.data(), bad.size()).ok, "bad magic rejected");
        RootSignature sig;
        sig.params = {consts(1)};
        const std::vector<uint8_t> bytes = encode_canonical(sig);
        check(!decode_canonical(bytes.data(), bytes.size() - 1).ok,
              "truncated canonical rejected");
        std::vector<uint8_t> trailed = bytes;
        trailed.push_back(0);
        check(!decode_canonical(trailed.data(), trailed.size()).ok,
              "trailing bytes rejected");
    }

    // RTS0 hypothesis: a hand-built blob per the documented layout parses.
    // Hand-built RTS0 payload in the offset-table layout the maintainer
    // described: header, parameter table, then payloads at their offsets.
    {
        const std::vector<uint8_t> payload = build_payload();
        const DecodeResult d = parse_rts0(payload.data(), payload.size());
        check(d.ok, "hand-built RTS0 parses");
        check(d.sig.params.size() == 2 && d.sig.params[0].num_values == 3 &&
                  d.sig.flags == 0x03000000u,
              "RTS0 model matches hand-built blob");
        const PackResult r = pack(d.sig);
        check(r.ok && r.dword_count == 3 + 2, "parsed RTS0 packs to 5 dwords");
    }

    // RTS0 controls: unknown version, wild offsets and truncation fail loudly.
    {
        std::vector<uint8_t> blob;
        put_u32(blob, 9);  // unknown version
        put_u32(blob, 0);
        put_u32(blob, 0);
        put_u32(blob, 0);
        put_u32(blob, 0);
        put_u32(blob, 0);
        const DecodeResult d = parse_rts0(blob.data(), blob.size());
        check(!d.ok, "unknown RTS0 version rejected");
        std::vector<uint8_t> cut = {1, 0};
        check(!parse_rts0(cut.data(), cut.size()).ok, "truncated RTS0 rejected");
        std::vector<uint8_t> wild = build_payload();
        // Corrupt the params offset to point past the end.
        wild[8] = 0xFF;
        wild[9] = 0xFF;
        wild[10] = 0xFF;
        wild[11] = 0xFF;
        check(!parse_rts0(wild.data(), wild.size()).ok, "wild offset rejected");
    }

    // DXBC container walk: a hand-built container with one RTS0 chunk parses
    // through the chunk table (same path the MS serializer blob takes).
    {
        const std::vector<uint8_t> payload = build_payload();
        std::vector<uint8_t> blob;
        blob.insert(blob.end(), {'D', 'X', 'B', 'C'});
        blob.insert(blob.end(), 16, 0);  // hash
        put_u32(blob, 1);  // version
        put_u32(blob, 0);  // file size (patched below)
        put_u32(blob, 1);  // 1 chunk
        put_u32(blob, 36);  // chunk offset: 4 magic + 16 hash + 4 ver + 4 size + 4 count + 4 offset
        blob.insert(blob.end(), {'R', 'T', 'S', '0'});
        put_u32(blob, static_cast<uint32_t>(payload.size()));
        blob.insert(blob.end(), payload.begin(), payload.end());
        const uint32_t total = static_cast<uint32_t>(blob.size());
        blob[24] = static_cast<uint8_t>(total);
        blob[25] = static_cast<uint8_t>(total >> 8);
        blob[26] = static_cast<uint8_t>(total >> 16);
        blob[27] = static_cast<uint8_t>(total >> 24);
        const DecodeResult d = parse_rts0(blob.data(), blob.size());
        check(d.ok, "DXBC container walk finds RTS0");
        check(d.sig.params.size() == 2 && d.sig.params[0].num_values == 3,
              "walked RTS0 model matches payload");
    }

    // RTS0 descriptor tables: the payload is {num_ranges, ranges_offset} and the ranges sit at that offset, as the
    // PC serializer writes them (root_signature_oracle_test); here a 1.1 table with two 24-byte ranges placed after
    // a gap, so reading them inline after the count would fail.
    {
        std::vector<uint8_t> p;
        put_u32(p, 2);   // version 1.1
        put_u32(p, 1);   // 1 param
        put_u32(p, 24);  // params table offset
        put_u32(p, 0);   // 0 samplers
        put_u32(p, 0);
        put_u32(p, 0);   // flags
        put_u32(p, 0);   // entry 0: descriptor table
        put_u32(p, 5);   // pixel
        put_u32(p, 36);  // table payload
        put_u32(p, 2);   // 2 ranges
        put_u32(p, 52);  // at 52
        put_u32(p, 0xdeadbeefu);  // gap
        put_u32(p, 0xdeadbeefu);
        // {type, count, base register, space, flags, offset}: 1.1 puts the flags before the table offset.
        for (const uint32_t v : {0u, 4u, 0u, 0u, 8u, 0xffffffffu,      // SRV x4, DATA_STATIC, appended
                                 2u, 1u, 3u, 1u, 1u, 6u})              // CBV x1 at b3 space1, DESCRIPTORS_VOLATILE, offset 6
            put_u32(p, v);
        const DecodeResult d = parse_rts0(p.data(), p.size());
        check(d.ok && d.sig.params.size() == 1 && d.sig.params[0].type == ParamType::Table &&
                  d.sig.params[0].visibility == Visibility::Pixel && d.sig.params[0].ranges.size() == 2 &&
                  d.sig.params[0].ranges[0].range_type == 0 && d.sig.params[0].ranges[0].num_descriptors == 4 &&
                  d.sig.params[0].ranges[1].range_type == 2 && d.sig.params[0].ranges[1].num_descriptors == 1,
              "RTS0 table ranges read from their offset");
        check(d.ok && d.sig.params[0].ranges.size() == 2 && d.sig.params[0].ranges[0].flags == 8 &&
                  d.sig.params[0].ranges[0].offset_in_table == kRangeOffsetAppend &&
                  d.sig.params[0].ranges[1].base_shader_register == 3 &&
                  d.sig.params[0].ranges[1].register_space == 1 && d.sig.params[0].ranges[1].flags == 1 &&
                  d.sig.params[0].ranges[1].offset_in_table == 6,
              "RTS0 1.1 ranges: register, space, flags, then table offset");
        std::vector<uint8_t> wild = p;
        wild[40] = 0xF0;  // ranges offset far past the payload
        check(!parse_rts0(wild.data(), wild.size()).ok, "RTS0 ranges offset past the payload rejected");
    }

    // RTS0 1.0 ranges have no flags dword: 20 bytes, the table offset right after the space.
    {
        std::vector<uint8_t> p;
        put_u32(p, 1);   // version 1.0
        put_u32(p, 1);   // 1 param
        put_u32(p, 24);  // params table offset
        put_u32(p, 0);   // 0 samplers
        put_u32(p, 0);
        put_u32(p, 0);   // flags
        put_u32(p, 0);   // entry 0: descriptor table
        put_u32(p, 0);   // all stages
        put_u32(p, 36);  // table payload
        put_u32(p, 2);   // 2 ranges
        put_u32(p, 44);  // right after the payload
        for (const uint32_t v : {1u, 2u, 5u, 0u, 0xffffffffu,    // UAV x2 at u5, appended
                                 3u, 1u, 2u, 4u, 9u})            // sampler x1 at s2 space4, offset 9
            put_u32(p, v);
        const DecodeResult d = parse_rts0(p.data(), p.size());
        check(d.ok && d.sig.params.size() == 1 && d.sig.params[0].ranges.size() == 2 &&
                  d.sig.params[0].ranges[0].range_type == 1 && d.sig.params[0].ranges[0].num_descriptors == 2 &&
                  d.sig.params[0].ranges[0].base_shader_register == 5 &&
                  d.sig.params[0].ranges[0].offset_in_table == kRangeOffsetAppend &&
                  d.sig.params[0].ranges[0].flags == 0 && d.sig.params[0].ranges[1].range_type == 3 &&
                  d.sig.params[0].ranges[1].base_shader_register == 2 &&
                  d.sig.params[0].ranges[1].register_space == 4 &&
                  d.sig.params[0].ranges[1].offset_in_table == 9 && d.sig.params[0].ranges[1].flags == 0,
              "RTS0 1.0 ranges: no flags, table offset at +16");
        std::vector<uint8_t> cut = p;
        cut.resize(cut.size() - 4);
        check(!parse_rts0(cut.data(), cut.size()).ok, "RTS0 1.0 range without its offset rejected");
    }

    // The canonical codec keeps every range field, and refuses the old codec version that dropped them.
    {
        RootSignature sig;
        sig.params = {table()};
        sig.params[0].ranges.push_back({2, 1, 7, 3, 4, 12});
        const std::vector<uint8_t> bytes = encode_canonical(sig);
        const DecodeResult d = decode_canonical(bytes.data(), bytes.size());
        check(d.ok && d.sig.params.size() == 1 && d.sig.params[0].ranges.size() == 2 &&
                  d.sig.params[0].ranges[1].range_type == 2 && d.sig.params[0].ranges[1].num_descriptors == 1 &&
                  d.sig.params[0].ranges[1].base_shader_register == 7 &&
                  d.sig.params[0].ranges[1].register_space == 3 && d.sig.params[0].ranges[1].flags == 4 &&
                  d.sig.params[0].ranges[1].offset_in_table == 12,
              "canonical codec round-trips every range field");
        std::vector<uint8_t> v1 = bytes;
        v1[4] = 1;  // codec version 1 stored ranges as {type, count} only
        check(!decode_canonical(v1.data(), v1.size()).ok, "canonical codec version 1 refused");
    }

    // Version 1.2: the canonical codec carries the static-sampler flags, and only for 1.2, so 1.0/1.1 encodings are
    // byte-for-byte what they were before 1.2 existed.
    {
        RootSignature sig;
        sig.version = Version::V1_2;
        sig.params = {consts(1)};
        StaticSampler s;
        s.border_color = 5;  // D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK_UINT
        s.flags = 1;         // D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR
        StaticSampler plain;
        plain.shader_register = 1;
        sig.static_samplers = {s, plain};
        const std::vector<uint8_t> v12 = encode_canonical(sig);
        const DecodeResult d = decode_canonical(v12.data(), v12.size());
        check(d.ok && d.sig.version == Version::V1_2 && d.sig.static_samplers.size() == 2 &&
                  d.sig.static_samplers[0].flags == 1 && d.sig.static_samplers[0].border_color == 5 &&
                  d.sig.static_samplers[1].flags == 0 && d.sig.static_samplers[1].shader_register == 1,
              "canonical 1.2 round-trips sampler flags");
        check(!decode_canonical(v12.data(), v12.size() - 4).ok, "canonical 1.2 without the last flags is truncated");
        sig.version = Version::V1_1;
        const std::vector<uint8_t> v11 = encode_canonical(sig);
        check(v11.size() + 2 * 4 == v12.size(), "canonical 1.1 has no sampler flags dword");
        const DecodeResult d11 = decode_canonical(v11.data(), v11.size());
        check(d11.ok && d11.sig.static_samplers[0].flags == 0, "canonical 1.1 decodes with flags 0");
    }

    // RTS0 version 3 (1.2): 1.1-style descriptor flags, then a 14-dword sampler whose last dword is its flags.
    {
        std::vector<uint8_t> p;
        put_u32(p, 3);   // version 1.2
        put_u32(p, 1);   // 1 param
        put_u32(p, 24);  // params table offset
        put_u32(p, 1);   // 1 sampler
        put_u32(p, 48);  // samplers offset
        put_u32(p, 0);   // flags
        put_u32(p, 2);   // entry 0: CBV
        put_u32(p, 0);
        put_u32(p, 36);
        put_u32(p, 9);   // register
        put_u32(p, 0);   // space
        put_u32(p, 4);   // descriptor flags (DATA_STATIC_WHILE_SET_AT_EXECUTE)
        // 1.2 static sampler with non-normalized coordinates: point/linear filter, clamp address modes, LOD 0.
        const uint32_t sampler[14] = {0x14, 3, 4, 3, 0, 1, 0, 0, 0, 0, 3, 1, 5, 2};
        for (const uint32_t v : sampler) put_u32(p, v);
        const DecodeResult d = parse_rts0(p.data(), p.size());
        check(d.ok, "hand-built RTS0 1.2 parses");
        check(d.ok && d.sig.version == Version::V1_2 && d.sig.params.size() == 1 &&
                  d.sig.params[0].descriptor_register == 9 && d.sig.static_samplers.size() == 1 &&
                  d.sig.static_samplers[0].filter == 0x14 && d.sig.static_samplers[0].max_anisotropy == 1 &&
                  d.sig.static_samplers[0].border_color == 0 && d.sig.static_samplers[0].shader_register == 3 &&
                  d.sig.static_samplers[0].register_space == 1 &&
                  d.sig.static_samplers[0].visibility == Visibility::Pixel &&
                  d.sig.static_samplers[0].flags == 2,
              "RTS0 1.2 sampler fields and flags");
        const DecodeResult d_unpack = unpack(p.data(), p.size());
        check(d_unpack.ok && d_unpack.sig.version == Version::V1_2, "unpack alias parses hand-built RTS0 1.2");
        std::vector<uint8_t> cut = p;
        cut.resize(cut.size() - 4);  // a 13-dword (1.0/1.1-sized) sampler in a 1.2 blob
        check(!parse_rts0(cut.data(), cut.size()).ok, "RTS0 1.2 sampler without flags rejected");
        std::vector<uint8_t> v4 = p;
        v4[0] = 4;
        check(!parse_rts0(v4.data(), v4.size()).ok, "RTS0 version 4 rejected");

        // --- Unpack / parse_rts0 Validation of Invalid Configurations ---
        // 1. RTS0 1.2 with unknown sampler flags rejected by unpack
        std::vector<uint8_t> p12_bad_flags = p;
        p12_bad_flags[48 + 52] = 4;  // corrupt sampler flags to unknown bit 4
        check(!unpack(p12_bad_flags.data(), p12_bad_flags.size()).ok,
              "unpack rejects RTS0 1.2 with unknown sampler flags");

        // 2. RTS0 1.2 with non-normalized coordinates and linear mip filter rejected by unpack
        std::vector<uint8_t> p12_bad_filter = p;
        p12_bad_filter[48 + 0] = 0x15;  // filter with linear mip
        check(!unpack(p12_bad_filter.data(), p12_bad_filter.size()).ok,
              "unpack rejects RTS0 1.2 non-normalized sampler with linear mip filter");

        // 3. RTS0 1.2 with non-normalized coordinates and wrap address mode rejected by unpack
        std::vector<uint8_t> p12_bad_addr = p;
        p12_bad_addr[48 + 4] = kAddressModeWrap;  // address_u = Wrap (1)
        check(!unpack(p12_bad_addr.data(), p12_bad_addr.size()).ok,
              "unpack rejects RTS0 1.2 non-normalized sampler with wrap AddressU");

        // 4. RTS0 1.2 with non-normalized coordinates and non-zero max_lod rejected by unpack
        std::vector<uint8_t> p12_bad_lod = p;
        p12_bad_lod[48 + 36] = 0x80;  // max_lod non-zero
        check(!unpack(p12_bad_lod.data(), p12_bad_lod.size()).ok,
              "unpack rejects RTS0 1.2 non-normalized sampler with non-zero max_lod");

        // 5. RTS0 1.2 with UINT border color flag and float opaque white border color rejected by unpack
        std::vector<uint8_t> p12_bad_uint = p;
        p12_bad_uint[48 + 52] = kSamplerFlagUintBorderColor;  // flags = 1
        p12_bad_uint[48 + 28] = kStaticBorderColorOpaqueWhite;  // border_color = 2 (float opaque white)
        check(!unpack(p12_bad_uint.data(), p12_bad_uint.size()).ok,
              "unpack rejects RTS0 1.2 with UINT border color flag and float opaque border color");

        // 6. RTS0 1.1 with sampler flags != 0 rejected by unpack
        std::vector<uint8_t> p11_sampler;
        put_u32(p11_sampler, 2);   // version 1.1
        put_u32(p11_sampler, 1);   // 1 param
        put_u32(p11_sampler, 24);  // params table offset
        put_u32(p11_sampler, 1);   // 1 sampler
        put_u32(p11_sampler, 48);  // samplers offset
        put_u32(p11_sampler, 0);   // flags
        put_u32(p11_sampler, 2);   // entry 0: CBV
        put_u32(p11_sampler, 0);
        put_u32(p11_sampler, 36);
        put_u32(p11_sampler, 9);   // register
        put_u32(p11_sampler, 0);   // space
        put_u32(p11_sampler, 0);   // flags
        // 13-dword sampler in 1.1: with UINT border color without flags (flags are 0 in 1.1)
        const uint32_t s11_bad[13] = {0, 3, 3, 3, 0, 1, 0, kStaticBorderColorOpaqueWhiteUint, 0, 0, 1, 0, 0};
        for (const uint32_t v : s11_bad) put_u32(p11_sampler, v);
        check(!unpack(p11_sampler.data(), p11_sampler.size()).ok,
              "unpack rejects RTS0 1.1 with UINT border color (requires 1.2 sampler flags)");

        // 7. RTS0 payload exceeding user SGPR budget (> 32 user SGPRs) rejected by unpack
        std::vector<uint8_t> p_spill;
        put_u32(p_spill, 2);   // version 1.1
        put_u32(p_spill, 17);  // 17 params (17 * 2 = 34 user SGPRs)
        put_u32(p_spill, 24);  // params table offset
        put_u32(p_spill, 0);   // 0 samplers
        put_u32(p_spill, 0);
        put_u32(p_spill, 0);
        const uint32_t payload_off = 24 + 17 * 12;
        for (uint32_t i = 0; i < 17; ++i) {
            put_u32(p_spill, 2);  // CBV
            put_u32(p_spill, 0);  // all
            put_u32(p_spill, payload_off);
        }
        put_u32(p_spill, 0);  // reg
        put_u32(p_spill, 0);  // space
        put_u32(p_spill, 0);  // flags
        check(!unpack(p_spill.data(), p_spill.size()).ok,
              "unpack rejects RTS0 payload exceeding 32 user SGPRs (spill unsupported)");
    }

    // --- Version 1.2 Static Sampler Validation Tests ---
    {
        RootSignature sig;
        sig.version = Version::V1_2;
        sig.params = {consts(1)};
        sig.static_samplers = {valid_uint_border_sampler(), valid_non_normalized_sampler()};
        check(pack(sig).ok, "version 1.2 static samplers with valid flags pack successfully");

        // Both flags combined on one sampler.
        StaticSampler both = valid_non_normalized_sampler();
        both.flags = kSamplerFlagUintBorderColor | kSamplerFlagNonNormalizedCoordinates;
        both.border_color = kStaticBorderColorOpaqueBlackUint;
        sig.static_samplers = {both};
        check(pack(sig).ok, "version 1.2 static sampler with both flags packs successfully");

        // Version 1.1 cannot have sampler flags.
        sig.version = Version::V1_1;
        check(!pack(sig).ok, "version 1.1 with sampler flags rejected by pack");

        // Version 1.0 cannot have sampler flags.
        sig.version = Version::V1_0;
        check(!pack(sig).ok, "version 1.0 with sampler flags rejected by pack");

        // Version 1.2 with unknown sampler flags.
        sig.version = Version::V1_2;
        StaticSampler bad_flags = valid_non_normalized_sampler();
        bad_flags.flags = 4;  // unknown flag bit
        sig.static_samplers = {bad_flags};
        check(!pack(sig).ok, "version 1.2 with unknown sampler flags rejected by pack");

        // UINT border color with float opaque black/white.
        StaticSampler bad_uint = valid_uint_border_sampler();
        bad_uint.border_color = kStaticBorderColorOpaqueBlack;
        sig.static_samplers = {bad_uint};
        check(!pack(sig).ok, "UINT border color sampler with float opaque black rejected");
        bad_uint.border_color = kStaticBorderColorOpaqueWhite;
        sig.static_samplers = {bad_uint};
        check(!pack(sig).ok, "UINT border color sampler with float opaque white rejected");

        // Float sampler (flags == 0) with UINT border color.
        StaticSampler bad_float;
        bad_float.flags = kSamplerFlagNone;
        bad_float.border_color = kStaticBorderColorOpaqueWhiteUint;
        sig.static_samplers = {bad_float};
        check(!pack(sig).ok, "float sampler with UINT border color rejected without UINT flag");
        bad_float.border_color = kStaticBorderColorOpaqueBlackUint;
        sig.static_samplers = {bad_float};
        check(!pack(sig).ok, "float sampler with UINT border black rejected without UINT flag");

        // Non-normalized coordinates violations:
        // 1. Min != Mag filter
        StaticSampler nn = valid_non_normalized_sampler();
        nn.filter = 0x10;  // min linear (1), mag point (0), mip point (0)
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with min != mag filter rejected");

        // 2. Linear MIP filter
        nn = valid_non_normalized_sampler();
        nn.filter = 0x15;  // min/mag linear, mip linear
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with linear mip filter rejected");

        // 3. Anisotropic filter
        nn = valid_non_normalized_sampler();
        nn.filter = 0x54;  // anisotropic bit 0x40
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with anisotropic filter rejected");

        // 4. Comparison filter reduction
        nn = valid_non_normalized_sampler();
        nn.filter = 0x94;  // reduction type 1 (comparison)
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with comparison reduction rejected");

        // 5. Invalid AddressU (Wrap)
        nn = valid_non_normalized_sampler();
        nn.address_u = kAddressModeWrap;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with wrap AddressU rejected");

        // 6. Invalid AddressU (Mirror)
        nn = valid_non_normalized_sampler();
        nn.address_u = kAddressModeMirror;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with mirror AddressU rejected");

        // 7. Invalid AddressV (Wrap)
        nn = valid_non_normalized_sampler();
        nn.address_v = kAddressModeWrap;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with wrap AddressV rejected");

        // 8. Invalid AddressV (MirrorOnce)
        nn = valid_non_normalized_sampler();
        nn.address_v = kAddressModeMirrorOnce;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with mirror-once AddressV rejected");

        // 9. AddressW can be Wrap (free per D3D12 / Vulkan)
        nn = valid_non_normalized_sampler();
        nn.address_u = kAddressModeClamp;
        nn.address_v = kAddressModeBorder;
        nn.address_w = kAddressModeWrap;
        sig.static_samplers = {nn};
        check(pack(sig).ok, "non-normalized sampler with wrap AddressW accepted (free)");

        // 10. Non-zero MinLOD
        nn = valid_non_normalized_sampler();
        nn.min_lod = 1.0f;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with non-zero min_lod rejected");

        // 11. Non-zero MaxLOD
        nn = valid_non_normalized_sampler();
        nn.max_lod = 5.0f;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with non-zero max_lod rejected");

        // 12. Non-zero MipLODBias
        nn = valid_non_normalized_sampler();
        nn.mip_lod_bias = 0.5f;
        sig.static_samplers = {nn};
        check(!pack(sig).ok, "non-normalized sampler with non-zero mip_lod_bias rejected");
    }

    // --- Downgrade Validation Tests (docs/d3d12_vulkan.md section 3.5) ---
    {
        RootSignature sig12;
        sig12.version = Version::V1_2;
        sig12.params = {consts(2), view(), table()};
        StaticSampler s0;
        s0.flags = kSamplerFlagNone;
        s0.border_color = kStaticBorderColorOpaqueWhite;
        StaticSampler s1;
        s1.flags = kSamplerFlagNone;
        s1.border_color = kStaticBorderColorTransparentBlack;
        sig12.static_samplers = {s0, s1};

        check(can_downgrade_to_1_1(sig12), "1.2 signature without sampler flags can downgrade to 1.1");
        RootSignature downgraded;
        check(downgrade_to_1_1(sig12, downgraded), "downgrade_to_1_1 succeeds on 1.2 without sampler flags");
        check(downgraded.version == Version::V1_1, "downgraded signature has version 1.1");
        check(downgraded.params.size() == 3 && downgraded.static_samplers.size() == 2,
              "downgraded signature preserves parameters and samplers");
        check(downgraded.static_samplers[0].flags == 0 && downgraded.static_samplers[1].flags == 0,
              "downgraded samplers have flags 0");

        // Empty 1.2 signature downgrades to empty 1.1
        RootSignature empty12;
        empty12.version = Version::V1_2;
        RootSignature empty11;
        check(can_downgrade_to_1_1(empty12) && downgrade_to_1_1(empty12, empty11) &&
                  empty11.version == Version::V1_1 && empty11.params.empty() && empty11.static_samplers.empty(),
              "empty 1.2 signature downgrades to empty 1.1");

        // 1.2 with UINT border color sampler flag cannot downgrade
        RootSignature sig_uint = sig12;
        sig_uint.static_samplers[0] = valid_uint_border_sampler();
        RootSignature untouched;
        untouched.version = Version::V1_0;
        check(!can_downgrade_to_1_1(sig_uint), "1.2 with UINT border color flag cannot downgrade");
        check(!downgrade_to_1_1(sig_uint, untouched) && untouched.version == Version::V1_0,
              "downgrade_to_1_1 returns false on UINT border color, output untouched");

        // 1.2 with non-normalized coordinates sampler flag cannot downgrade
        RootSignature sig_nn = sig12;
        sig_nn.static_samplers[1] = valid_non_normalized_sampler();
        check(!can_downgrade_to_1_1(sig_nn), "1.2 with non-normalized coordinates flag cannot downgrade");
        check(!downgrade_to_1_1(sig_nn, untouched) && untouched.version == Version::V1_0,
              "downgrade_to_1_1 returns false on non-normalized coordinates, output untouched");

        // 1.1 or 1.0 signatures cannot be downgraded
        RootSignature sig11 = sig12;
        sig11.version = Version::V1_1;
        check(!can_downgrade_to_1_1(sig11), "1.1 signature cannot be downgraded");
        check(!downgrade_to_1_1(sig11, untouched), "downgrade_to_1_1 on 1.1 returns false");
    }

    // --- Parameter Budgets and Validation Tests (docs/d3d12_vulkan.md section 3.5) ---
    {
        RootSignature exact_sgprs;
        for (int i = 0; i < 16; ++i) exact_sgprs.params.push_back(table());
        PackResult r_tables = pack(exact_sgprs);
        check(r_tables.ok && r_tables.dword_count == 32 && r_tables.d3d12_cost == 16,
              "16 descriptor tables fill 32 SGPRs and cost 16");

        // 16 tables + 1 constant exceeds 32 SGPRs (33 dwords) -> refused as spill
        RootSignature spill_tables = exact_sgprs;
        spill_tables.params.push_back(consts(1));
        PackResult r_spill = pack(spill_tables);
        check(!r_spill.ok && r_spill.error.find("spill") != std::string::npos,
              "16 tables + 1 constant (33 SGPRs) refused as spill");

        // More than 64 root parameters refused
        RootSignature over_64;
        for (int i = 0; i < 65; ++i) over_64.params.push_back(table());
        PackResult r_over64 = pack(over_64);
        check(!r_over64.ok && r_over64.error.find("64 root parameters") != std::string::npos,
              "65 root parameters refused by parameter count limit");

        // Static samplers consume 0 SGPR dwords and 0 cost:
        // 16 views (32 SGPRs) + 8 static samplers still fits 32 SGPRs!
        RootSignature views_and_samplers;
        for (int i = 0; i < 16; ++i) views_and_samplers.params.push_back(view());
        for (int i = 0; i < 8; ++i) views_and_samplers.static_samplers.push_back(StaticSampler{});
        PackResult r_samplers = pack(views_and_samplers);
        check(r_samplers.ok && r_samplers.dword_count == 32 && r_samplers.d3d12_cost == 32,
              "16 views + 8 static samplers fits exactly in 32 SGPRs (samplers cost 0 dwords)");

        // Descriptor table with 0 ranges refused
        RootSignature empty_table;
        RootParameter bad_tbl;
        bad_tbl.type = ParamType::Table;
        empty_table.params.push_back(bad_tbl);
        check(!pack(empty_table).ok, "descriptor table with 0 ranges refused");

        // Descriptor range with num_descriptors == 0 refused
        RootSignature zero_desc_range;
        RootParameter bad_range_tbl;
        bad_range_tbl.type = ParamType::Table;
        bad_range_tbl.ranges.push_back({0, 0});  // 0 descriptors
        zero_desc_range.params.push_back(bad_range_tbl);
        check(!pack(zero_desc_range).ok, "descriptor range with 0 descriptors refused");

        // Descriptor range with range_type > 3 refused
        RootSignature bad_range_type;
        RootParameter bad_type_tbl;
        bad_type_tbl.type = ParamType::Table;
        bad_type_tbl.ranges.push_back({4, 1});  // invalid range type
        bad_range_type.params.push_back(bad_type_tbl);
        check(!pack(bad_range_type).ok, "descriptor range with type > 3 refused");

        // Sampler range with DATA_* flags refused
        RootSignature bad_sampler_range;
        RootParameter s_tbl;
        s_tbl.type = ParamType::Table;
        s_tbl.ranges.push_back({3, 1, 0, 0, 8});  // range_type 3 (sampler) with DATA_STATIC (8)
        bad_sampler_range.params.push_back(s_tbl);
        check(!pack(bad_sampler_range).ok, "sampler descriptor range with DATA_* flags refused");
    }

    if (g_failures != 0) std::printf("%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
