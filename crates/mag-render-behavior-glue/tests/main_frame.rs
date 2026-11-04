//! One synthetic Main frame from guest memory, end to end:
//! hand-built guest memory → adapter-core readers → clean-room plan → `SceneBuilder` →
//! `mag_render_native::validate_scene_packet_v3`.
//!
//! The frame has items in groups 0 (instanced wind), 1 (static), 4 (ripple), 11 (skinned) and
//! 12 (static, after the group-twelve setup), and a stencil-mask producer pass. Every byte is
//! synthetic.
//!
//! Where no public specification fixes how a value is found, the harness uses a stated
//! convention, marked `GAP:`. Those are the open specification items this harness exposed.
#![allow(unsafe_code)]

use mag_render_abi::packet::Viewport;
use mag_render_abi::{Source, TextureLayout, VertexAttribute};
use mag_render_adapter_core::cohort::{CohortBuilder, CohortHeader};
use mag_render_adapter_core::geometry_bindings::{
    VertexBinding as CoreBinding, decode_gpu_attribute, decode_output_bindings,
};
use mag_render_adapter_core::guest_memory::GuestSnapshot;
use mag_render_adapter_core::main_flow::{MainItemRoute, main_item_route};
use mag_render_adapter_core::material_programs::{
    FragmentSourceReference, ProgramBudget, decode_fragment_program_source,
    decode_technique_programs, decode_vertex_program_source,
};
use mag_render_adapter_core::mesh_sources::{MeshSource, PartitionChain};
use mag_render_adapter_core::partition_inputs::{
    NonInstancedSourceBases, copy_noninstanced_inputs,
};
use mag_render_behavior::effects::{
    self, AttachmentImage, LoadOp, ModuleConstants, SurfaceBinding,
};
use mag_render_behavior::finish::{CarriedState, FinishSources, finish};
use mag_render_behavior::group_twelve::run_group_twelve;
use mag_render_behavior::group_zero::{GroupZeroAdmission, GroupZeroInputs, admit_group_zero};
use mag_render_behavior::material_values::{
    Identity, ItemInputs, MaterialValueState, PassValues, TransformRecord, read_parameter_column,
    read_technique,
};
use mag_render_behavior::noninstanced::{
    DeformationAdmission, DeformationInputs, admit_noninstanced,
};
use mag_render_behavior::producers::{ProducerInputs, acquire, globals, renderer};
use mag_render_behavior::program_compilation::{
    CompiledPair, EffectiveValues, ProgramCompiler, ShaderControls, TechniqueSource, Variant,
};
use mag_render_behavior::raster::{
    BlendConstant, KNOWN_ALL, RasterDrawInputs, Topology, translate,
};
use mag_render_behavior::scene_plan::{
    self, CohortHeader as PlanHeader, DrawSources, PlanStep, ProducerEvent, SetupReferences,
    SourceIdentity, check_association, traverse,
};
use mag_render_behavior::setup::{
    MainSetupRecord, PipelineState, decode_inherited_snapshot, inherited_snapshot_methods,
};
use mag_render_behavior::skinning::{SkinningAdmission, SkinningInputs, admit_skinning};
use mag_render_behavior::surfaces::{
    ViewportConstants, map_viewport, select, source_viewport, table_position,
};
use mag_render_behavior::texture::TextureUnitRecord;
use mag_render_behavior::transitions::{TransitionSources, transition, water_rows};
use mag_render_behavior::vertex_inputs::{
    AttributeList, AuxiliarySet, GroupZeroPreparation, NonInstancedPreparation, Prepared,
    VertexBinding, VertexDraw, pack_vertex_inputs,
};
use mag_render_behavior_glue::SnapshotMemory;
use mag_render_behavior_glue::abi::*;
use mag_render_behavior_glue::cohort::{cohort_header, traversal_items};
use mag_render_behavior_glue::deformation::{
    DeformationKey, DeformationStore, Prepared as Deformed, group_zero, noninstanced, skinned,
};
use mag_render_behavior_glue::packet::{CallerWork, PacketAssembler};
use mag_render_behavior_glue::programs::{RsxTranslator, alpha_reference, program_usage};
use std::sync::Arc;

// --- Guest memory ----------------------------------------------------------------------------

const BASE: u32 = 0x1_0000;
const SIZE: usize = 0x8_0000;

struct Guest(Vec<u8>);

impl Guest {
    fn new() -> Self {
        Self(vec![0; SIZE])
    }
    fn at(&mut self, address: u32, length: usize) -> &mut [u8] {
        let start = (address - BASE) as usize;
        &mut self.0[start..start + length]
    }
    fn bytes(&mut self, address: u32, bytes: &[u8]) {
        self.at(address, bytes.len()).copy_from_slice(bytes);
    }
    fn u8(&mut self, address: u32, value: u8) {
        self.bytes(address, &[value]);
    }
    fn u16(&mut self, address: u32, value: u16) {
        self.bytes(address, &value.to_be_bytes());
    }
    fn u32(&mut self, address: u32, value: u32) {
        self.bytes(address, &value.to_be_bytes());
    }
    fn f32(&mut self, address: u32, value: f32) {
        self.u32(address, value.to_bits());
    }
    fn words(&mut self, address: u32, words: &[u32]) {
        for (i, w) in words.iter().enumerate() {
            self.u32(address + 4 * i as u32, *w);
        }
    }
    fn floats(&mut self, address: u32, values: &[f32]) {
        for (i, v) in values.iter().enumerate() {
            self.f32(address + 4 * i as u32, *v);
        }
    }
}

// Layout of the synthetic image.
const TOC: u32 = 0x8_0000;
const RENDERER: u32 = 0x2_0000;
const PARAMS: u32 = 0x2_4000;
const JITTER: u32 = 0x2_5800;
const COMMON_VC: u32 = 0x2_5a00;
const VP_TABLE: u32 = 0x2_6000;
const FP_TABLE: u32 = 0x2_6400;
const VECTORS: u32 = 0x2_7000;
const PRODUCER_VP: u32 = 0x2_8000;
const PRODUCER_FP: u32 = 0x2_8200;
const SETUP_RECORD: u32 = 0x2_f000;
const PASS: u32 = 0x3_0000;
const GROUP12_DESCRIPTORS: u32 = 0x3_1000;
const ENVIRONMENT: u32 = 0x3_1800;
const MATERIALS: u32 = 0x3_2000;
const CHUNK: u32 = 0x3_8000;
const FRAGMENT_BANK: u32 = 0x3_c000;
const MESHES: u32 = 0x4_0000;
const LAYOUTS: u32 = 0x4_1000;
const PARTITIONS: u32 = 0x4_2000;
const STREAMS: u32 = 0x5_0000;
const RESOURCE_BASE: u32 = 0x6_0000;
const PALETTE: u32 = 0x6_8000;
const COLOUR_POOL: u32 = 0x6_c000;
const STENCIL_BANK: u32 = 0x4000;
const RESTORE_BANK: u32 = 0x41c;

const W: u32 = 64;
const H: u32 = 64;

/// The items: (group, material index, mesh index).
const ITEMS: [(u32, u32); 5] = [(0, 1), (1, 0), (4, 0), (11, 2), (12, 0)];

fn surface_record(position: u32) -> [u32; 21] {
    let mut w = [0u32; 21];
    w[1] = W << 16;
    w[2] = H << 16;
    w[3] = 8 | (2 << 5);
    w[4] = 4 * W;
    w[8] = 0x10_0000 * (position + 1);
    w[12] = 0xfeed_0000;
    w[16] = 0x80_0000 + 0x10_0000 * position;
    w[17] = 4 * W;
    w[18] = 0xfeed_0000;
    w
}

/// One-instruction vertex program: input 0 to the position output.
fn mov() -> [u32; 4] {
    let src = 0x1b02;
    [
        (1 << 30) | (63 << 15),
        (1 << 22) | (src >> 9),
        (src & 511) << 23,
        (15 << 13) | 1,
    ]
}

fn half_exchanged(words: &[u32]) -> Vec<u8> {
    words
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect()
}

/// A fragment program with one inline constant row.
fn constant_fragment() -> Vec<u8> {
    half_exchanged(&[
        (1 << 24) | (15 << 9) | 1,
        2 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
        1f32.to_bits(),
        0,
        0,
        1f32.to_bits(),
    ])
}

/// A fragment program that samples unit 0.
fn texture_fragment() -> Vec<u8> {
    half_exchanged(&[
        (0x17 << 24) | (15 << 9) | (4 << 13) | 1,
        1 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
    ])
}

/// A vertex program object: header, one instruction at offset 32.
fn vertex_object() -> Vec<u8> {
    let mut b = vec![0u8; 48];
    b[0..2].copy_from_slice(&1u16.to_be_bytes());
    b[4..8].copy_from_slice(&1u32.to_be_bytes());
    b[8..12].copy_from_slice(&1u32.to_be_bytes());
    b[18..20].copy_from_slice(&32u16.to_be_bytes());
    for (i, w) in mov().iter().enumerate() {
        b[32 + 4 * i..36 + 4 * i].copy_from_slice(&w.to_be_bytes());
    }
    b
}

/// Positions of the partitions' vertices (float4 rows).
const VERTICES: [[f32; 4]; 3] = [
    [-0.5, -0.5, 0.25, 1.0],
    [0.5, -0.5, 0.25, 1.0],
    [0.0, 0.5, 0.25, 1.0],
];

fn build_guest() -> Guest {
    let mut g = Guest::new();

    // Globals below the TOC.
    for (displacement, value) in [
        (globals::RENDERER, RENDERER),
        (globals::PARAMS, PARAMS),
        (globals::JITTER, JITTER),
        (globals::COMMON_VC, COMMON_VC),
        (globals::VP_TABLE, VP_TABLE),
        (globals::FP_TABLE, FP_TABLE),
        (globals::V_ONE, VECTORS),
        (globals::S_DEPTH, VECTORS + 0x10),
        (globals::BASIS_A, VECTORS + 0x20),
        (globals::BASIS_B, VECTORS + 0x30),
        (globals::BASIS_C, VECTORS + 0x40),
    ] {
        g.u32(TOC - displacement, value);
    }
    for (displacement, value) in [
        (globals::Z0, 0.0),
        (globals::Z1, 1.0),
        (globals::T_LO, -1.0),
        (globals::T_HI, 1.0),
        (globals::T_FAR, 3.0),
        (globals::T_UVFAR, 2.0),
        (globals::VP_HX, 0.5),
        (globals::VP_HY, -0.5),
        (globals::CLIP_MIN, 0.0),
        (globals::CLIP_MAX, 1.0),
    ] {
        g.f32(TOC - displacement, value);
    }

    // Renderer: surface table, descriptors, bank zero enabled.
    for position in 0..8 {
        g.words(
            RENDERER + renderer::SURFACE_TABLE + 84 * position,
            &surface_record(position),
        );
    }
    let s = table_position(1, 0) as u32;
    g.words(
        RENDERER + renderer::SURFACE_DESCRIPTORS + 32 * s,
        &[
            0x10_0000 * (s + 1),
            0x0001_8521,
            0x0003_0303,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            (W << 16) | H,
            4 * W,
        ],
    );
    g.u32(RENDERER + renderer::SURFACE_BRANCH, 1);
    g.u8(RENDERER + renderer::STENCIL_BRANCH, 1);

    // Producer programs: stencil mask (vertex 3, fragment 0x41).
    g.u32(VP_TABLE + 12 * 3 + 4, PRODUCER_VP);
    g.bytes(PRODUCER_VP, &vertex_object());
    let fragment = texture_fragment();
    g.u32(FP_TABLE + 16 * 0x41 + 4, PRODUCER_FP);
    g.u32(FP_TABLE + 16 * 0x41 + 8, 24 + fragment.len() as u32);
    g.u32(PRODUCER_FP, fragment.len() as u32);
    g.u32(PRODUCER_FP + 8, 0x40);
    g.u32(PRODUCER_FP + 16, 24);
    g.bytes(PRODUCER_FP + 24, &fragment);

    // Main setup record: lists on, initial bank 0 (stencil mask), restore bank.
    g.u8(SETUP_RECORD + 0x4c, 1);
    g.u32(SETUP_RECORD + 0x50, STENCIL_BANK);
    g.u32(SETUP_RECORD + 0x6c, RESTORE_BANK);

    // Pass object.
    g.f32(PASS + 0x68, 2.0);
    g.floats(
        PASS + 0x80,
        &[
            1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
        ],
    );
    g.f32(PASS + 0xd0, 7.0);
    g.u32(PASS + 0x110, GROUP12_DESCRIPTORS);
    g.floats(PASS + 0x118, &[10.0, 20.0, 30.0, 1.0, 2.0, 4.0]);
    g.u32(PASS + 0x144, 3);
    g.floats(PASS + 0x19c, &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]);
    g.u32(PASS + 0x1bc, GROUP12_DESCRIPTORS + 0x40);
    g.u32(PASS + 0x1c0, GROUP12_DESCRIPTORS + 0x60);
    g.f32(PASS + 0x1dc, 1.0);
    g.u32(PASS + 0x22c, 2);
    g.u32(PASS + 0x230, 8);
    g.u32(PASS + 0x238, ENVIRONMENT);
    g.floats(PASS + 0x260, &[1.0, 0.0, 0.5, 0.5, 12.5]);
    g.words(
        GROUP12_DESCRIPTORS,
        &[
            0x10,
            0x0001_8521,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            0x0010_0010,
            0,
            64,
            0,
            0,
            0,
        ],
    );
    g.words(
        GROUP12_DESCRIPTORS + 0x40,
        &[
            0x20,
            0x0001_8521,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            0x0010_0010,
            64,
        ],
    );
    g.words(
        GROUP12_DESCRIPTORS + 0x60,
        &[
            0x30,
            0x0001_8521,
            0,
            0x8000_0000,
            0xaae4,
            0x0202_0000,
            0x0010_0010,
            64,
        ],
    );
    // View environment rows E0..E3 at +0x10..+0x40.
    for row in 0..4u32 {
        let mut values = [0.0f32; 4];
        values[row as usize] = 1.0;
        g.floats(ENVIRONMENT + 0x10 + 16 * row, &values);
    }

    // Materials: 0 ordinary, 1 group-zero (flag bit 0), 2 skinned (flag 0x20).
    for (index, flags) in [(0u32, 0u32), (1, 0x01), (2, 0x20)] {
        let m = MATERIALS + 0x400 * index;
        g.floats(
            m,
            &[0.5, 1.0, 0.0, 1.0, 0.5, 0.25, 2.0, 0.0, 0.25, 1.0, 4.0, 0.0],
        );
        g.u16(m + 0x44, 0x100);
        g.u16(m + 0x46, 0x200);
        g.u16(m + 0x48, 0x80);
        g.u16(m + 0x4c, 1);
        g.u32(m + 0x58, flags);
        // Technique 0: membership bit 0, flags bit 7 (transform rows).
        g.u8(m + 0x80 + 1, 0);
        g.bytes(m + 0x88, &(1u64 << 7).to_be_bytes());
        // One 4-float parameter to fragment slot 0; its destination word is chunk +0x100.
        g.u32(m + 0x100 + 4, 1);
        g.u8(m + 0x100 + 10, 7);
        g.u16(m + 0x100 + 12, 0x100);
        g.floats(m + 0x200, &[0.25, 0.5, 0.75, 1.0]);
    }

    // Technique chunk (shared): objects a, b, vertex and fragment descriptors, programs.
    g.u32(CHUNK, 0x10);
    g.u32(CHUNK + 0x10 + 12, 0x10);
    g.u32(CHUNK + 0x20 + 12, 0x20);
    g.u32(CHUNK + 0x20 + 16, 0x40);
    g.u32(CHUNK + 0x40 + 16, 0x40);
    g.u32(CHUNK + 0x60 + 16, 0x60);
    g.bytes(CHUNK + 0x80, &vertex_object());
    let microcode = constant_fragment();
    g.u32(CHUNK + 0xc0, microcode.len() as u32);
    g.u32(CHUNK + 0xc0 + 8, 0x40);
    g.u32(CHUNK + 0xc0 + 12, 0x2);
    // Relocation directory: slot 0 → list at +0x30 naming inline row 1 (byte offset 16).
    g.u32(CHUNK + 0xc0 + 24, 0x30);
    g.u16(CHUNK + 0xc0 + 0x30, 1);
    g.u16(CHUNK + 0xc0 + 0x32, 1);
    g.u32(CHUNK + 0x100, 3 << 24);
    g.bytes(FRAGMENT_BANK, &microcode);

    // Meshes, layouts and partitions, one per item.
    for (index, &(group, _)) in ITEMS.iter().enumerate() {
        let index = index as u32;
        let mesh = MESHES + 0x100 * index;
        let layout = LAYOUTS + 0x40 * index;
        let partition = PARTITIONS + 0x300 * index;
        let streams = STREAMS + 0x1000 * index;
        g.floats(mesh + 0x20, &[1.0, 2.0, 3.0, 4.0]);
        g.u32(mesh + 0x34, 2);
        g.u32(mesh + 0x3c, layout);
        g.u32(mesh + 0x50, partition);
        g.u32(
            mesh + 0x9c,
            if matches!(group, 1 | 12) { 0x1000 } else { 0 },
        );
        g.u8(mesh + 0xa2, 1);
        g.u16(mesh + 0xbc, group as u16);
        // Custom output layout: one float3 position (semantic 1) in register 0.
        g.bytes(layout, &[1, 16, 0, 0, 0, 0, 0, 0, 0, 2, 3, 1, 12, 0, 0, 0]);
        // Partition streams (GAP: stream roles are a harness convention).
        let stream = |slot: u32| partition + 0x30 + 8 * slot;
        let positions: Vec<f32> = VERTICES.iter().flatten().copied().collect();
        g.floats(streams, &positions);
        g.words(stream(0), &[48, streams]);
        g.floats(streams + 0x100, &[0.0, 0.0, 1.0, 0.0].repeat(3));
        g.words(stream(1), &[48, streams + 0x100]);
        g.floats(streams + 0x200, &[1.0, 0.5, 0.25, 0.0].repeat(3));
        g.words(stream(2), &[48, streams + 0x200]);
        g.floats(streams + 0x300, &[1.0, 0.0, 0.0, 1.0].repeat(3));
        g.words(stream(6), &[48, streams + 0x300]);
        g.bytes(streams + 0x400, &[0, 0, 0, 1, 0, 2]);
        g.words(stream(7), &[6, streams + 0x400]);
        g.u32(partition + 0xfc, 1);
        if group == 11 {
            // One palette matrix in segment 0 at PALETTE + 0x10; influences (stream 5),
            // configuration (stream 19).
            g.u16(partition + 0x14, 0x10);
            g.words(stream(3), &[48, 0]);
            g.floats(
                PALETTE + 0x10,
                &[1.0, 0.0, 0.0, 0.25, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0],
            );
            g.bytes(streams + 0x500, &[255, 0, 0, 0, 0, 0, 0, 0].repeat(3));
            g.words(stream(5), &[24, streams + 0x500]);
            let mut config = [0u8; 16];
            config[0] = 0x83;
            config[6] = 17;
            config[12..].fill(0xff);
            g.bytes(streams + 0x600, &config);
            g.words(stream(19), &[16, streams + 0x600]);
        }
    }
    // Group-zero instance rows (two instances) and colour pool.
    g.u16(RESOURCE_BASE, 0x3c00);
    g.u16(RESOURCE_BASE + 6, 0x3c00);
    g.u16(RESOURCE_BASE + 14, 0x3c00);
    g.u16(RESOURCE_BASE + 16 + 2, 0x3800);
    g.u16(RESOURCE_BASE + 16 + 6, 0x3800);
    g.u16(RESOURCE_BASE + 16 + 14, 0x3c00);
    g.bytes(COLOUR_POOL, &[200, 100, 50, 255].repeat(8));
    g
}

/// The 112-byte transform record of item `index`.
fn transform(index: usize, group: u32) -> [u8; 112] {
    let mut t = [0u8; 112];
    let mut put =
        |at: usize, value: f32| t[at..at + 4].copy_from_slice(&value.to_bits().to_be_bytes());
    put(0x0c, 1.0);
    put(0x10, index as f32);
    put(0x1c, 1.0);
    for row in 0..4 {
        put(0x20 + 20 * row, 1.0);
    }
    if group == 0 {
        t[0x60..0x64].copy_from_slice(&(0x8000_0000u32).to_be_bytes());
        t[0x64..0x68].copy_from_slice(&(0x8000_0000u32 | COLOUR_POOL).to_be_bytes());
        t[0x6c] = 2;
    }
    t
}

fn item_mbi(index: usize, group: u32, material: u32) -> [u8; 32] {
    let mut mbi = [0u8; 32];
    mbi[0] = (group << 4) as u8;
    // GAP: the draw-batch 24-bit material address (bytes 6..8, low byte zero) is used as the
    // primary material location; the full material-source specification is not public.
    let address = MATERIALS + 0x400 * material;
    mbi[6..9].copy_from_slice(&address.to_be_bytes()[0..3]);
    mbi[12..16].copy_from_slice(&(index as u32).to_be_bytes());
    if group == 11 {
        mbi[20..24].copy_from_slice(&PALETTE.to_be_bytes());
    }
    mbi
}

// --- Plan inputs -------------------------------------------------------------------------------

fn be(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn snapshot_state() -> (PipelineState, [TextureUnitRecord; 16]) {
    // A fully known inherited state: front face CCW, cull back, blend factors one/zero and add,
    // depth LEQUAL, alpha ALWAYS, stencil faces ALWAYS/KEEP, every mask on, no offset, no bounds.
    let values = [
        (0x1834, 0x901),
        (0x1830, 0x405),
        (0x314, 0x0001_0001),
        (0x318, 0),
        (0x320, 0x8006_8006),
        (0xa6c, 0x203),
        (0x308, 0x207),
        (0x324, 0x0101_0101),
        (0x370, 0xffff),
        (0x388, 0x3f80_0000),
    ];
    let mut pairs: Vec<(u32, u32)> = inherited_snapshot_methods()
        .into_iter()
        .map(|m| (m, 0))
        .collect();
    for (method, value) in values {
        pairs.iter_mut().find(|(m, _)| *m == method).unwrap().1 = value;
    }
    for base in [0x32c, 0x34c] {
        for (i, value) in [0xff, 0x207, 0, 0xff, 0x1e00, 0x1e00, 0x1e00]
            .into_iter()
            .enumerate()
        {
            pairs
                .iter_mut()
                .find(|(m, _)| *m == base + 4 * i as u32)
                .unwrap()
                .1 = value;
        }
    }
    let state = decode_inherited_snapshot(&pairs).unwrap();
    (state.pipeline, state.texture_units)
}

fn shader_viewport(v: &mag_render_behavior::surfaces::NativeViewport) -> ([u32; 4], [u32; 4]) {
    (
        [1f32.to_bits(), 1f32.to_bits(), v.z_scale.to_bits(), 0],
        [0, 0, v.z_offset.to_bits(), 0],
    )
}

fn complete_raster(state: &PipelineState) -> mag_render_abi::packet::RasterState {
    let raster = translate(
        state,
        &RasterDrawInputs {
            topology: Some(Topology::Triangles),
            samples: Some(1),
            colour_attachment: 0,
            blend_constant: BlendConstant::Unobserved,
            bias_clamp: None,
            obligations: 0,
        },
    )
    .unwrap();
    assert_eq!(
        (raster.known_fields, raster.unresolved_obligations),
        (KNOWN_ALL, 0),
        "effective raster state"
    );
    raster
}

fn ne(indices: &[u32]) -> Vec<u8> {
    indices.iter().flat_map(|i| i.to_ne_bytes()).collect()
}

/// Adds compiled programs and a pipeline.
fn add_pipeline(
    builder: &mut SceneBuilder,
    pair: &CompiledPair,
    sampled: bool,
    stride: u32,
    attributes: Vec<VertexAttribute>,
    colour_format: u32,
    state: mag_render_abi::packet::RasterState,
) -> u32 {
    let vertex = builder.add_program(OwnedProgram {
        stage: 0,
        flags: 0,
        language: LANGUAGE_HLSL,
        source: pair.vertex.bytes.clone(),
        entry: b"main".to_vec(),
        buffers: vec![BindingLayoutRecord::uniform(
            0,
            pair.vertex_uniform.len() as u64,
        )],
        textures: Vec::new(),
        samplers: Vec::new(),
        vertex_locations: pair.vertex.vertex_input_locations.clone(),
        threads: [0; 3],
    });
    let fragment = builder.add_program(OwnedProgram {
        stage: 1,
        flags: 0,
        language: LANGUAGE_HLSL,
        source: pair.fragment.bytes.clone(),
        entry: b"main".to_vec(),
        buffers: vec![BindingLayoutRecord::uniform(
            1,
            pair.fragment_uniform.len() as u64,
        )],
        textures: if sampled {
            vec![TextureSlotRecord {
                slot: 0,
                dimension: DIMENSION_2D,
                sample_kind: 0,
            }]
        } else {
            Vec::new()
        },
        samplers: if sampled { vec![(0, 0)] } else { Vec::new() },
        vertex_locations: Vec::new(),
        threads: [0; 3],
    });
    builder.add_pipeline(OwnedPipeline {
        record: PipelineRecord {
            vertex,
            fragment,
            stride,
            colour_format,
            depth_format: 15,
            binding_model: 1,
            state,
        },
        attributes,
        textures: if sampled {
            vec![TextureLayout {
                stage: 1,
                texture_slot: 0,
                sampler_slot: 0,
                format: 1,
                comparison: 0,
                reserved: 0,
            }]
        } else {
            Vec::new()
        },
    })
}

fn uniforms(builder: &mut SceneBuilder, pair: &CompiledPair) -> Vec<(u32, u32, u32, u64, u64)> {
    let vu = builder.add_buffer(OwnedBuffer::Bytes(pair.vertex_uniform.clone()));
    let fu = builder.add_buffer(OwnedBuffer::Bytes(pair.fragment_uniform.clone()));
    vec![
        (0, 0, vu, 0, pair.vertex_uniform.len() as u64),
        (1, 1, fu, 0, pair.fragment_uniform.len() as u64),
    ]
}

#[test]
fn one_main_frame_from_guest_memory() {
    let guest = build_guest();
    let snapshot = GuestSnapshot::new(BASE, &guest.0).unwrap();
    let memory = SnapshotMemory(GuestSnapshot::new(BASE, &guest.0).unwrap());

    // Producer inputs and the Main setup record.
    let inputs: ProducerInputs = acquire(&memory, TOC).unwrap();
    let setup =
        MainSetupRecord::decode(&snapshot.copy_bytes(SETUP_RECORD as u64, 160).unwrap()).unwrap();
    let references = SetupReferences {
        lists_enabled: setup.lists_enabled,
        initial: setup.initial,
        group_thirteen: setup.group_thirteen,
        group_fifteen: setup.group_fifteen,
    };
    let pass = snapshot.copy_bytes(PASS as u64, 0x2b0).unwrap();
    let pass_control = be(&pass, 0x144);
    let transitions = TransitionSources {
        lists_enabled: setup.lists_enabled,
        depth_control: setup.depth_control,
        initial: [0; 3],
        group_thirteen: 0,
        group_fifteen: [0; 2],
        stencil_use: be(&pass, 0x114) >> 24 != 0,
        shader_packer: be(&pass, 0x220) != 0,
    };

    // Cohort through the adapter core's builder.
    let mut counts = [[0u32; 16]; 4];
    for &(group, _) in &ITEMS {
        counts[0][group as usize] += 1;
    }
    let header = CohortHeader {
        vm: 1,
        source_invocation: 2,
        ppu_owner: 3,
        pass_occurrence: 4,
        execution_update: 1,
        camera: 0,
        group_counts: counts,
    };
    let mut cohort = CohortBuilder::new(header, 64, 1 << 20).unwrap();
    for (index, &(group, material)) in ITEMS.iter().enumerate() {
        cohort
            .push(
                0,
                index as u16,
                item_mbi(index, group, material),
                Some(transform(index, group)),
            )
            .unwrap();
    }
    let cohort = cohort.finish().unwrap();
    let source = SourceIdentity {
        engine_update: 1,
        cohort: 2,
        publication: 9,
        pass_occurrence: 4,
    };
    let plan_header: PlanHeader = cohort_header(cohort.header());

    // Material entries: mesh group from the mesh record, the item's own transform.
    let meshes: Vec<MeshSource> = (0..ITEMS.len())
        // GAP: the mesh record of an item is found by its mesh index; the location rule is not
        // public, so the harness places mesh `i` at MESHES + 0x100·i.
        .map(|i| MeshSource::copy(&snapshot, MESHES + 0x100 * i as u32).unwrap())
        .collect();
    let materials: Vec<_> = cohort
        .items()
        .iter()
        .zip(&meshes)
        .map(|(item, mesh)| {
            Some(Some(DrawSources {
                group: mesh.material_group() as u32,
                transform: item.transform.unwrap(),
                pass_control,
            }))
        })
        .collect();
    let items = traversal_items(cohort.items(), &materials).unwrap();
    let ordinals: Vec<usize> = (0..items.len()).collect();
    check_association(
        &source,
        &plan_header,
        (1, 1),
        (1, 1),
        items.len(),
        &ordinals,
        &ordinals,
        true,
    )
    .unwrap();
    let (steps, stats) = traverse(&items, &references, pass_control).unwrap();
    assert_eq!(stats.emitted_items, 5);

    // The adapter core's route agrees with the planner's for every item.
    for item in &items {
        let group = item.material.unwrap().unwrap().group;
        assert!(matches!(
            main_item_route(group as u16, pass_control),
            Ok(MainItemRoute::Prepare(_))
        ));
    }

    // Packet arena: the Main surface, the stencil-mask target and their images.
    let mut builder = SceneBuilder::new(Source {
        engine_update: 1,
        cohort: 2,
        publication: 9,
        pass_occurrence: 4,
    });
    let main_surface = select(&inputs.renderer, 0, 1).unwrap();
    let mask_target = select(&inputs.renderer, 1, 0).unwrap();
    let main_colour = builder.add_image(OwnedImage::attachment(1, W, H));
    let main_depth = builder.add_image(OwnedImage::attachment(15, W, H));
    let mask_depth = builder.add_image(OwnedImage::attachment(15, W, H));
    builder.output_image = main_colour;
    let binding = |address, colour, depth| SurfaceBinding {
        address,
        physical: (W, H),
        colour_format: 1,
        depth_format: 15,
        colour: Some(AttachmentImage {
            image: colour,
            load: LoadOp::Load,
        }),
        depth: Some(AttachmentImage {
            image: depth,
            load: LoadOp::Load,
        }),
        stencil: Some(AttachmentImage {
            image: depth,
            load: LoadOp::Load,
        }),
        depth_initialised: true,
        stencil_initialised: true,
    };
    let mut bindings = vec![
        binding(main_surface.address, main_colour, main_depth),
        binding(mask_target.address, main_colour, mask_depth),
    ];
    for entry in &inputs.renderer.surface_table {
        if bindings.iter().all(|b| b.address != entry.address) {
            bindings.push(binding(entry.address, main_colour, main_depth));
        }
    }
    let main_binding = bindings[0];

    let constants = ViewportConstants::of(&inputs);
    let main_viewport = map_viewport(
        &source_viewport(&main_surface, &constants).unwrap(),
        (W, H),
        (1, 1),
    )
    .unwrap();

    // Carried state.
    let (mut pipeline_state, mut units) = snapshot_state();
    let mut values = MaterialValueState::new();
    values.begin_window();
    let mut compiler = ProgramCompiler::new(RsxTranslator, true);
    let mut store = DeformationStore::new();
    let mut assembler = PacketAssembler::<OwnedDraw>::new();
    let budget = ProgramBudget {
        source_bytes: 1 << 16,
        vertex_instructions: 512,
        static_constant_rows: 512,
        fragment_bytes: 1 << 16,
    };
    let chunk = snapshot.copy_bytes(CHUNK as u64, 0x140).unwrap();
    let technique_programs = decode_technique_programs(&chunk, budget).unwrap();
    let FragmentSourceReference::AssetBankOffset(bank_offset) =
        technique_programs.fragment.reference
    else {
        panic!("asset bank reference")
    };
    // GAP: the asset-bank base that turns the fragment reference into microcode bytes is not in
    // a public specification; the harness uses FRAGMENT_BANK.
    let microcode = snapshot
        .copy_bytes(
            (FRAGMENT_BANK + bank_offset) as u64,
            technique_programs.fragment.semantic_bytes as usize,
        )
        .unwrap();
    let technique_source = TechniqueSource {
        vertex_instructions: technique_programs.vertex.instructions.clone(),
        load_slot: technique_programs.vertex.load_slot as u32,
        declared_input_mask: technique_programs.vertex.input_mask as u16,
        declared_output_mask: technique_programs.vertex.output_mask,
        transform_word: technique_programs.vertex.unresolved_transform_word,
        fragment_bytes: microcode.clone(),
        semantic_bytes: microcode.len(),
        transfer_bytes: microcode.len(),
        shader_control: technique_programs.fragment.shader_control,
        texcoord_controls: technique_programs.fragment.texcoord_controls,
        directory: technique_programs.fragment.relocation_directory.clone(),
    };

    let mut current_group = -1i32;
    let mut producer_passes = 0;
    let mut producer_viewport = None;
    let mut item_draws = Vec::new();
    for step in steps {
        let Some(work) = assembler.apply(step, &main_binding, Some(1)).unwrap() else {
            continue;
        };
        match work {
            CallerWork::Step(PlanStep::Event(ProducerEvent::Initial { slot, target })) => {
                let routed = effects::route_event(
                    ProducerEvent::Initial { slot, target },
                    &references,
                    Some(1),
                )
                .unwrap();
                assert_eq!(routed, effects::Routed::StencilMask);
                let programs = &inputs.programs.stencil;
                let fragment_file = programs.fragment.as_ref().unwrap();
                let usage = program_usage(fragment_file.microcode()).unwrap();
                let result = effects::stencil_mask(
                    &inputs.renderer,
                    programs.both_present(),
                    &usage,
                    &bindings,
                    &ModuleConstants::of(&inputs),
                    &pipeline_state,
                    &units,
                )
                .unwrap();
                let pass = result.pass.clone().unwrap();
                // Compile the producer's programs as the common contract says.
                let vertex =
                    decode_vertex_program_source(&programs.vertex.as_ref().unwrap().bytes, budget)
                        .unwrap();
                let fragment_source = decode_fragment_program_source(
                    &fragment_file.bytes,
                    budget,
                    FragmentSourceReference::AssetBankOffset(0),
                )
                .unwrap();
                let microcode = fragment_file.microcode().to_vec();
                let technique = TechniqueSource {
                    vertex_instructions: vertex.instructions,
                    load_slot: vertex.load_slot as u32,
                    declared_input_mask: vertex.input_mask as u16,
                    declared_output_mask: vertex.output_mask,
                    transform_word: vertex.unresolved_transform_word,
                    semantic_bytes: microcode.len(),
                    transfer_bytes: microcode.len(),
                    fragment_bytes: microcode,
                    shader_control: fragment_source.shader_control,
                    texcoord_controls: fragment_source.texcoord_controls,
                    directory: Vec::new(),
                };
                // Shader controls see unit 0 as it is during the draw (enabled), not after it.
                let mut during = result.units;
                if let Some(words) = &mut during[0].compact {
                    words[3] = inputs.renderer.surface_descriptors[table_position(1, 0)].value[3];
                }
                let controls = scene_plan::shader_controls(
                    &pass.state,
                    &during,
                    usage.sampled_units,
                    (1, 1),
                    8,
                    [0; 3],
                    0,
                    &alpha_reference,
                )
                .unwrap();
                let mut controls: ShaderControls = controls.controls;
                controls.enabled_arrays = effects::PRODUCER_ARRAYS;
                let (scale, offset) = shader_viewport(&pass.viewport.native);
                let pair = compiler
                    .compile(
                        &technique,
                        Variant::Source,
                        &controls,
                        &EffectiveValues {
                            vertex_bank: vec![None; 512],
                            viewport_scale: scale,
                            viewport_offset: offset,
                            current: [None; 16],
                            fragment_writes: Vec::new(),
                            alpha_window: [0; 4],
                        },
                    )
                    .unwrap();
                let raster = effects::producer_raster(&pass.state, None).unwrap();
                let pipeline = add_pipeline(
                    &mut builder,
                    &pair,
                    true,
                    32,
                    // The packet's attributes are exactly the program's reflected inputs: position
                    // at byte 0 and texture coordinate at byte 16 of each 32-byte vertex.
                    pair.vertex
                        .vertex_input_locations
                        .iter()
                        .map(|&location| VertexAttribute {
                            location,
                            format: 4,
                            offset: if location == 8 { 16 } else { 0 },
                            reserved: 0,
                        })
                        .collect(),
                    0,
                    raster,
                );
                let geometry = &pass.geometry;
                let vb = builder.add_buffer(OwnedBuffer::Bytes(geometry.vertices.clone()));
                let ib = builder.add_buffer(OwnedBuffer::Bytes(ne(&geometry.indices)));
                let texture = &pass.textures[0];
                let view = builder.add_view(texture.view.to_packet(main_colour), DIMENSION_2D);
                let sampler = builder.add_sampler(texture.sampler);
                let buffers = uniforms(&mut builder, &pair);
                let identity = assembler.add_producer_pass(
                    &pass,
                    OwnedDraw {
                        source_item: 0,
                        source_partition: 0,
                        obligations: 0,
                        pipeline,
                        index_count: geometry.indices.len() as u32,
                        vertices: (vb, 0, geometry.vertices.len() as u64),
                        indices: (ib, 0, 4 * geometry.indices.len() as u64),
                        buffers,
                        textures: vec![(1, 0, 0, view, sampler)],
                    },
                );
                assembler.passes.last_mut().unwrap().draws[0].source_item = identity;
                producer_passes += 1;
                producer_viewport = Some(packet_viewport(&pass.viewport.native));
                pipeline_state = result.state;
                units = result.units;
            }
            CallerWork::Step(PlanStep::Event(ProducerEvent::Group12Textures)) => {
                let out = run_group_twelve(
                    &memory,
                    setup.lists_enabled,
                    PASS,
                    PASS,
                    &units,
                    &pipeline_state,
                )
                .unwrap()
                .unwrap();
                units = out.units;
                pipeline_state = out.state;
            }
            CallerWork::Step(PlanStep::Event(ProducerEvent::Finish { previous_group })) => {
                let mut carried = CarriedState {
                    pipeline: pipeline_state,
                    units,
                    current: main_surface,
                    viewport: None,
                };
                finish(
                    previous_group,
                    Some(&FinishSources {
                        transitions,
                        restore: setup.restore,
                    }),
                    Some(1),
                    &inputs.renderer,
                    &bindings,
                    &constants,
                    &mut carried,
                    &mut |_, _| Ok(()),
                )
                .unwrap();
                pipeline_state = carried.pipeline;
                assert_eq!(carried.current.role, 2);
            }
            CallerWork::Step(PlanStep::Event(other)) => panic!("unexpected event {other:?}"),
            CallerWork::Step(PlanStep::Transition { from, to }) => {
                transition(&mut pipeline_state, from, to as i32, &transitions).unwrap();
                current_group = to as i32;
            }
            CallerWork::Step(PlanStep::WaterRows) => {
                let rows = water_rows(&memory, PASS).unwrap();
                values.bank.write(46, rows[0]).unwrap();
                values.bank.write(47, rows[1]).unwrap();
            }
            CallerWork::Step(other) => panic!("unexpected step {other:?}"),
            CallerWork::Prepare { item, group } => {
                let cohort_item = &cohort.items()[item];
                let mesh = &meshes[item];
                let mbi = cohort_item.mbi;
                let transform_bytes = cohort_item.transform.unwrap();
                let material = (u32::from(mbi[6]) << 24)
                    | (u32::from(mbi[7]) << 16)
                    | (u32::from(mbi[8]) << 8);
                let material_bytes = snapshot.copy_bytes(material as u64, 0x60).unwrap();
                let material_flags = be(&material_bytes, 0x58);
                let (technique, parameters) = read_technique(&memory, material, CHUNK, 0).unwrap();
                let parameter_column =
                    read_parameter_column(&memory, technique.flags, &mbi, be(&pass, 0x140))
                        .unwrap();
                let static_rows: Vec<(u32, [u32; 4])> = technique_programs
                    .vertex
                    .static_constants
                    .iter()
                    .map(|c| (c.slot, c.words))
                    .collect();
                let item_inputs = ItemInputs {
                    identity: Identity {
                        material,
                        technique: CHUNK,
                        selector: 0,
                        group,
                    },
                    technique,
                    parameters: &parameters,
                    material_class: material_bytes[0x56],
                    material_flags,
                    mbi,
                    transform: Some(TransformRecord::from_bytes(&transform_bytes)),
                    parameter_column,
                    mesh_constant: std::array::from_fn(|lane| be(mesh.bytes(), 0x20 + 4 * lane)),
                    pass: PassValues {
                        depth_anchor: be(&pass, 0x68),
                        depth_scale: be(&pass, 0x1dc),
                        view: std::array::from_fn(|i| be(&pass, 0x80 + 4 * i)),
                        lane3: be(&pass, 0xd0),
                        distance_words: std::array::from_fn(|i| be(&pass, 0x118 + 4 * i)),
                        parameter_column_base: be(&pass, 0x140),
                        control: pass_control,
                    },
                    static_rows: &static_rows,
                    // GAP: fragment built-in rows are not specified publicly; none are added.
                    builtin_rows: &[],
                };
                values.compose(&item_inputs, &memory).unwrap();

                // Geometry through the adapter core's readers.
                let chain = PartitionChain::copy(&snapshot, be(mesh.bytes(), 0x50), 4).unwrap();
                let partition = &chain.partitions()[0];
                let palette_base = be(&mbi, 20) & !0xf;
                let streams = copy_noninstanced_inputs(
                    partition,
                    NonInstancedSourceBases {
                        palette_base,
                        extra_stream: 0,
                        environment: 0,
                    },
                    1 << 20,
                    &mut |request| snapshot.copy_bytes(request.address as u64, request.bytes),
                )
                .unwrap();
                let float4 = CoreBinding {
                    declaration: 0,
                    semantic: 0,
                    slot: 0,
                    format: 2,
                    components: 4,
                    stride: 16,
                    byte_offset: 0,
                    encoded_bytes: 16,
                };
                // GAP: which partition stream holds which attribute bank, and its codec, is not in
                // a public specification. Convention: streams 0, 1, 2 and 6 hold float4 positions,
                // normals, weights and tangents; stream 7 holds u16 indices.
                let bank = |slot: usize| {
                    decode_gpu_attribute(&float4, streams.inputs()[slot].bytes(), 3, 1 << 16)
                        .unwrap()
                };
                let (positions, normals, weights, tangents) = (bank(0), bank(1), bank(2), bank(6));
                let raw_indices: Vec<u16> = streams.inputs()[7]
                    .bytes()
                    .chunks(2)
                    .map(|c| u16::from_be_bytes([c[0], c[1]]))
                    .collect();
                let layout = snapshot
                    .copy_bytes(mesh.vertex_layout_address() as u64, 16)
                    .unwrap();
                let outputs: Vec<VertexBinding> = decode_output_bindings(&layout)
                    .unwrap()
                    .iter()
                    .map(|b| VertexBinding {
                        declaration: b.declaration as u32,
                        semantic: b.semantic as u32,
                        slot: b.slot as u32,
                        format: b.format as u32,
                        components: b.components as u32,
                    })
                    .collect();
                let key = DeformationKey {
                    cohort: 2,
                    item: item as u32,
                    partition: 0,
                };
                let material_rows: [[u32; 4]; 3] = std::array::from_fn(|r| {
                    std::array::from_fn(|l| be(&material_bytes, 16 * r + 4 * l))
                });
                let wind: [u32; 5] = std::array::from_fn(|i| be(&pass, 0x260 + 4 * i));
                let origin: [u32; 3] = std::array::from_fn(|i| be(&transform_bytes, 0x10 + 4 * i));
                let mut draw = VertexDraw {
                    output_bindings: Some(outputs),
                    auxiliary: Some(AuxiliarySet::default()),
                    primary: AttributeList::Absent,
                    secondary: AttributeList::Absent,
                    positions: &positions,
                    raw_indices: &raw_indices,
                    direct: None,
                    group_zero: Prepared::Absent,
                    non_instanced: Prepared::Absent,
                };
                let shared = match group {
                    0 => {
                        admit_group_zero(&GroupZeroAdmission {
                            draw_batch_byte0: mbi[0],
                            pass_control,
                            pass_mode: be(&pass, 0x230),
                            pass_workers: be(&pass, 0x22c),
                            material_flags,
                            output_selector: 0,
                            instance_count: transform_bytes[0x6c],
                            instance_location: be(&transform_bytes, 0x60),
                            auxiliary_slot12: false,
                            colour_frequency: be(mesh.bytes(), 0x34) & 0xffff,
                            batch_max: be(partition.bytes(), 0xfc),
                        })
                        .unwrap();
                        let count = transform_bytes[0x6c] as usize;
                        // GAP: the geometry resource base and the colour pool's address mapping
                        // are not public; the harness uses RESOURCE_BASE and a direct address.
                        let rows_bytes = snapshot
                            .copy_bytes(
                                (RESOURCE_BASE + (be(&transform_bytes, 0x60) & 0x7fff_ffff)) as u64,
                                16 * count,
                            )
                            .unwrap();
                        let instances: Vec<[u16; 8]> = rows_bytes
                            .chunks(16)
                            .map(|r| {
                                std::array::from_fn(|i| {
                                    u16::from_be_bytes([r[2 * i], r[2 * i + 1]])
                                })
                            })
                            .collect();
                        let pool_bytes = snapshot
                            .copy_bytes((be(&transform_bytes, 0x64) & 0x7fff_ffff) as u64, 32)
                            .unwrap();
                        let pool: Vec<[u8; 4]> = pool_bytes
                            .chunks(4)
                            .map(|c| [c[0], c[1], c[2], c[3]])
                            .collect();
                        let environment = be(&pass, 0x238);
                        let env: [[u32; 4]; 4] = std::array::from_fn(|r| {
                            std::array::from_fn(|l| {
                                be(
                                    &snapshot
                                        .copy_bytes((environment + 0x10 + 16 * r as u32) as u64, 16)
                                        .unwrap(),
                                    4 * l,
                                )
                            })
                        });
                        let inputs = GroupZeroInputs {
                            wind,
                            material: [material_rows[0], material_rows[1]],
                            object: std::array::from_fn(|i| be(&transform_bytes, 4 * i)),
                            fade: [0, 0],
                            environment: env,
                            instances: &instances,
                            colour_frequency: be(mesh.bytes(), 0x34) & 0xffff,
                            batch_max: be(partition.bytes(), 0xfc),
                            colour_pool: &pool,
                            positions: &positions,
                            normals: &normals,
                            weights: &weights,
                            indices: &raw_indices,
                        };
                        Some(store.prepare(key, || group_zero(&inputs, 1 << 16)))
                    }
                    4 => {
                        assert_eq!(
                            admit_noninstanced(&DeformationAdmission {
                                mesh_group: group as u16,
                                draw_batch_byte0: mbi[0],
                                pass_address: PASS,
                                draw_batch_byte18: mbi[0x18],
                                draw_batch_byte1b: mbi[0x1b],
                                palette_lengths: [
                                    be(partition.bytes(), 0x48),
                                    be(partition.bytes(), 0x50)
                                ],
                                pass_mode: be(&pass, 0x230),
                                pass_workers: be(&pass, 0x22c),
                                material_flags,
                                has_tangents: true,
                            }),
                            Ok(4)
                        );
                        let inputs = DeformationInputs {
                            group,
                            wind,
                            material: material_rows,
                            origin,
                            positions: &positions,
                            normals: &normals,
                            weights: &weights,
                            tangents: Some(&tangents),
                        };
                        Some(store.prepare(key, || noninstanced(&inputs, 1 << 16)))
                    }
                    11 => {
                        let location = admit_skinning(&SkinningAdmission {
                            mesh_group: group as u16,
                            draw_batch_byte0: mbi[0],
                            mesh_flags: be(mesh.bytes(), 0x9c),
                            object_byte_6f: transform_bytes[0x6f],
                            material_flags,
                            pass_address: PASS,
                            draw_batch_byte18: mbi[0x18],
                            draw_batch_byte1b: mbi[0x1b],
                            draw_batch_palette: be(&mbi, 20),
                            pass_mode: be(&pass, 0x230),
                            pass_workers: be(&pass, 0x22c),
                            configuration: streams.inputs()[19].bytes().to_vec(),
                            segment_offsets: [
                                u16::from_be_bytes([
                                    partition.bytes()[0x14],
                                    partition.bytes()[0x15],
                                ]),
                                u16::from_be_bytes([
                                    partition.bytes()[0x16],
                                    partition.bytes()[0x17],
                                ]),
                            ],
                            segment_lengths: [
                                be(partition.bytes(), 0x48),
                                be(partition.bytes(), 0x50),
                            ],
                        })
                        .unwrap();
                        let segment = |s: usize| {
                            let (address, length) = location.segments[s];
                            if length == 0 {
                                Vec::new()
                            } else {
                                snapshot
                                    .copy_bytes(address as u64, length as usize)
                                    .unwrap()
                            }
                        };
                        let (s0, s1) = (segment(0), segment(1));
                        let f = |rows: &[[u32; 4]]| -> Vec<[f32; 4]> {
                            rows.iter().map(|r| r.map(f32::from_bits)).collect()
                        };
                        let (p, n, t) = (f(&positions), f(&normals), f(&tangents));
                        let inputs = SkinningInputs {
                            segments: [&s0, &s1],
                            influences: streams.inputs()[5].bytes(),
                            positions: &p,
                            normals: &n,
                            tangents: &t,
                            object: std::array::from_fn(|i| be(&transform_bytes, 4 * i)),
                        };
                        Some(store.prepare(key, || skinned(&inputs, 1 << 16)))
                    }
                    _ => None,
                };
                // The depth pre-pass would ask for the same key and receive the same result.
                if let Some(shared) = &shared {
                    assert!(Arc::ptr_eq(
                        shared,
                        &store.prepare(key, || unreachable!("prepared once"))
                    ));
                }
                let skinned_rows;
                let (zero_positions, zero_colours, zero_indices);
                match shared.as_deref() {
                    Some(Ok(Deformed::GroupZero(out))) => {
                        zero_positions = out.positions.clone();
                        zero_colours = out.colours.clone();
                        zero_indices = out.indices.clone();
                        draw.group_zero = Prepared::Present(GroupZeroPreparation {
                            positions: &zero_positions,
                            indices: &zero_indices,
                            colours: &zero_colours,
                            auxiliary: AuxiliarySet::default(),
                            gpu: None,
                        });
                    }
                    Some(Ok(Deformed::Ripple(out))) => {
                        skinned_rows = out.positions.clone();
                        draw.non_instanced = Prepared::Present(NonInstancedPreparation {
                            positions: &skinned_rows,
                            normals: &[],
                            weights: &[],
                            tangents: None,
                            mesh_group: group,
                            geometry_mode: 8,
                            configuration_byte6: 0,
                            palette_controls: [0, 0],
                        });
                    }
                    Some(Ok(Deformed::Skinned(out))) => {
                        // GAP: vertex-inputs has no skinned preparation case; the skinned banks are
                        // packed as a non-instanced preparation.
                        skinned_rows = out.positions.iter().map(|r| r.map(f32::to_bits)).collect();
                        draw.non_instanced = Prepared::Present(NonInstancedPreparation {
                            positions: &skinned_rows,
                            normals: &[],
                            weights: &[],
                            tangents: None,
                            mesh_group: group,
                            geometry_mode: 8,
                            configuration_byte6: 0,
                            palette_controls: [0, 0],
                        });
                    }
                    Some(Err(error)) => panic!("deformation refused: {error:?}"),
                    None => {}
                }

                // Programs with this item's values.
                let array_mask =
                    mag_render_behavior::vertex_inputs::effective_array_mask(&draw).unwrap();
                let controls = scene_plan::shader_controls(
                    &pipeline_state,
                    &units,
                    0,
                    (1, 1),
                    8,
                    [0; 3],
                    0,
                    &alpha_reference,
                )
                .unwrap();
                let mut shader_controls = controls.controls;
                shader_controls.enabled_arrays = array_mask;
                let (scale, offset) = shader_viewport(&main_viewport);
                let pair = compiler
                    .compile(
                        &technique_source,
                        Variant::Source,
                        &shader_controls,
                        &EffectiveValues {
                            vertex_bank: (0..512).map(|r| values.bank.row(r)).collect(),
                            viewport_scale: scale,
                            viewport_offset: offset,
                            current: values.current,
                            fragment_writes: values.fragment_writes.clone(),
                            alpha_window: controls.alpha_constant,
                        },
                    )
                    .unwrap();
                let required = pair.required_arrays(array_mask).unwrap();
                let packed = pack_vertex_inputs(&draw, required, 1 << 20).unwrap();
                let raster = complete_raster(&pipeline_state);
                let attributes = packed
                    .attributes
                    .iter()
                    .map(|&(location, offset)| VertexAttribute {
                        location,
                        format: 4,
                        offset,
                        reserved: 0,
                    })
                    .collect();
                let pipeline = add_pipeline(
                    &mut builder,
                    &pair,
                    false,
                    packed.stride,
                    attributes,
                    1,
                    raster,
                );
                let vb = builder.add_buffer(OwnedBuffer::Bytes(packed.vertices.clone()));
                let ib = builder.add_buffer(OwnedBuffer::Bytes(ne(&packed.indices)));
                let buffers = uniforms(&mut builder, &pair);
                assembler
                    .add_draw(OwnedDraw {
                        source_item: item as u64,
                        source_partition: 0,
                        obligations: 0,
                        pipeline,
                        index_count: packed.indices.len() as u32,
                        vertices: (vb, 0, packed.vertices.len() as u64),
                        indices: (ib, 0, 4 * packed.indices.len() as u64),
                        buffers,
                        textures: Vec::new(),
                    })
                    .unwrap();
                let first_x = f32::from_le_bytes(packed.vertices[0..4].try_into().unwrap());
                item_draws.push((group, packed.indices.len(), first_x));
            }
        }
    }
    assert_eq!(current_group, 12);
    assert_eq!(producer_passes, 1);
    assert_eq!(
        item_draws.iter().map(|d| d.0).collect::<Vec<_>>(),
        vec![0, 1, 4, 11, 12]
    );
    // Group 0 expands two instances; group 1 draws the decoded positions unchanged; group 11
    // applies the palette's 0.25 translation.
    assert_eq!(item_draws[0].1, 6);
    assert_eq!(item_draws[1].2, VERTICES[0][0]);
    assert_eq!(item_draws[3].2, VERTICES[0][0] + 0.25);

    // Pass viewports: the producer pass uses its own, Main passes the Main surface's.
    let producer = assembler
        .passes
        .iter()
        .position(|p| {
            matches!(
                p.source,
                mag_render_behavior_glue::packet::PassSource::Producer { .. }
            )
        })
        .unwrap();
    let viewports: Vec<Viewport> = (0..assembler.passes.len())
        .map(|i| {
            if i == producer {
                producer_viewport.unwrap()
            } else {
                packet_viewport(&main_viewport)
            }
        })
        .collect();
    builder.add_assembled(assembler, &viewports).unwrap();
    assert_eq!(builder.items.len(), 6);

    let view = builder.view();
    // SAFETY: the descriptor points into `builder` and `view`, both alive and unchanged.
    let status = unsafe { mag_render_native::validate_scene_packet_v3(view.desc()) };
    assert_eq!(status, Ok(()));
}
