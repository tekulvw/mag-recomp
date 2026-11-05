//! Explicit source-semantic POC: omit base-RGB times deferred-lighting only.
//! Source sample, inline alpha/KIL and HDR/output-alpha operations stay intact.
//! Caller additionally proves the optimized native interface uses only unit0.
use super::{CompileError, compiler};
pub(super) struct SpecializedFragment {
    pub program: compiler::HlslProgram,
    pub outputs: [u8; 16],
}
#[path = "material_fragment_slice.rs"]
mod slice;
fn unsupported<T>() -> Result<T, CompileError> {
    Err(CompileError::Decode(compiler::ProgramError::Unsupported))
}
fn temporary(text: &str) -> bool {
    text.strip_prefix("h[")
        .or_else(|| text.strip_prefix("r["))
        .and_then(|s| s.strip_suffix(']'))
        .is_some_and(|n| {
            !n.is_empty()
                && n.bytes().all(|b| b.is_ascii_digit())
                && n.parse::<usize>().is_ok_and(|n| n < 48)
        })
}
fn store<'a>(line: &'a str, suffix: &str) -> Option<&'a str> {
    line.split_once("; ")?.1.strip_suffix(suffix)
}
fn writes(text: &str, target: &str, lanes: &str) -> bool {
    text.split(';').any(|statement| {
        statement
            .trim()
            .split_once(" = ")
            .and_then(|(lhs, _)| lhs.strip_prefix(target))
            .and_then(|mask| mask.strip_prefix('.'))
            .is_some_and(|mask| mask.chars().any(|lane| lanes.contains(lane)))
    })
}
// Exact optional source-emitter transforms, copied verbatim. They may read
// output RGBA and fp_alpha, but never an untracked temporary/input register.
fn valid_epilogue(source: &str, base: &str) -> bool {
    let marker = format!("    float4 _o = {base};\n");
    let Some((_, mut tail)) = source.split_once(&marker) else {
        return false;
    };
    let mut output = "(_o == _o) ? _o : (float4)0";
    let srgb = format!(
        "    float4 _rsx_color_out = {output};\n    _rsx_color_out = _rsx_srgb_output(_rsx_color_out);\n"
    );
    if let Some(rest) = tail.strip_prefix(&srgb) {
        tail = rest;
        output = "_rsx_color_out";
    }
    let alpha =
        format!("    float4 _rsx_out = {output};\n    const float _rsx_alpha_ref = fp_alpha.x;\n");
    if let Some(rest) = tail.strip_prefix(&alpha) {
        let Some((condition, rest)) = rest
            .strip_prefix("    if (!(")
            .and_then(|s| s.split_once(")) discard;\n"))
        else {
            return false;
        };
        if condition != "false"
            && !["<", "==", "<=", ">", "!=", ">="]
                .iter()
                .any(|op| condition == format!("_rsx_out.a {op} _rsx_alpha_ref"))
        {
            return false;
        }
        tail = rest;
        output = "_rsx_out";
    }
    tail == format!("    return {output};\n}}\n")
}
pub(super) fn specialize(
    mut program: compiler::HlslProgram,
) -> Result<SpecializedFragment, CompileError> {
    let text = &program.source;
    if !text.contains("float4 main(PSInput input) : SV_TARGET {")
        || text.contains("SV_Depth")
        || text.contains("SV_TARGET1")
        || text.contains("if (cc")
        || text.contains("while (")
    {
        return unsupported();
    }
    let lines: Vec<&str> = text.lines().collect();
    let samples: Vec<_> = lines
        .iter()
        .enumerate()
        .filter(|(_, l)| l.contains("rsx_tex0.Sample(rsx_samp0, "))
        .collect();
    let [(sample_index, sample)] = samples.as_slice() else {
        return unsupported();
    };
    let Some(base) = store(sample, ".xyzw = _v.xyzw; }") else {
        return unsupported();
    };
    if !temporary(base) || !valid_epilogue(text, base) {
        return unsupported();
    }
    let mut product = None;
    for (index, line) in lines.iter().enumerate().skip(sample_index + 1) {
        let prefix = format!("    {{ float4 _v = (float4)((({base}).xyzw) * ((");
        let suffix = format!(").xyzw)); {base}.xyz = _v.xyz; }}");
        if let Some(factor) = line
            .strip_prefix(&prefix)
            .and_then(|l| l.strip_suffix(&suffix))
        {
            if product.is_some() || !temporary(factor) || factor == base {
                return unsupported();
            }
            product = Some((index, *line, factor));
        }
    }
    let Some((product_index, product_line, factor)) = product else {
        return unsupported();
    };
    // The source base lanes must still be exactly the identified sample.
    if lines[sample_index + 1..product_index]
        .iter()
        .any(|l| writes(l, base, "xyzw"))
    {
        return unsupported();
    }
    // The RGB multiplier must contain a distinct actual texture sample, not an
    // arbitrary material tint. Later RGB writes must retain that factor.
    let lighting_sample = lines[..product_index]
        .iter()
        .enumerate()
        .rev()
        .find(|(_, l)| {
            l.contains(".Sample(rsx_samp")
                && !l.contains("rsx_tex0.Sample(")
                && store(l, ".xyz = _v.xyz; }") == Some(factor)
        })
        .map(|(i, _)| i);
    let Some(lighting_sample) = lighting_sample else {
        return unsupported();
    };
    for line in &lines[lighting_sample + 1..product_index] {
        if line.contains(&format!("; {factor}.xyz ="))
            && !line.contains(&format!("(({factor}).xyzw)"))
        {
            return unsupported();
        }
    }
    let mut alpha_discard = false;
    for (i, line) in lines.iter().enumerate().skip(sample_index + 1) {
        let prefix = format!("    {{ float4 _v = (float4)((float4)((({base}).");
        let Some(rest) = line.strip_prefix(&prefix) else {
            continue;
        };
        let Some((swizzle, cutoff)) = rest.split_once(") <  ((fp_constants[") else {
            continue;
        };
        if swizzle.len() != 4
            || !swizzle
                .bytes()
                .all(|b| matches!(b, b'x' | b'y' | b'z' | b'w'))
        {
            continue;
        }
        let Some((row, tail)) = cutoff.split_once(']') else {
            continue;
        };
        if row.is_empty() || !row.bytes().all(|b| b.is_ascii_digit()) || !tail.ends_with(")));") {
            continue;
        }
        let Some(cc_line) = lines.get(i + 1) else {
            continue;
        };
        let cc_line = cc_line.trim();
        let Some(cc) = cc_line
            .strip_prefix("cc0.")
            .or_else(|| cc_line.strip_prefix("cc1."))
        else {
            continue;
        };
        let Some((lane, rhs)) = cc.split_once(" = _v.") else {
            continue;
        };
        if lane.len() != 1 || rhs != format!("{lane};") {
            continue;
        }
        let lane_index = match lane {
            "x" => 0,
            "y" => 1,
            "z" => 2,
            "w" => 3,
            _ => continue,
        };
        if swizzle.as_bytes()[lane_index] != b'w' {
            continue;
        }
        // RGB lighting may precede source KIL. It cannot affect the tested
        // sample alpha: require that exact lane to survive from the sample to
        // this comparison, irrespective of RGB instruction scheduling.
        if lines[sample_index + 1..i]
            .iter()
            .any(|l| writes(l, base, "w"))
        {
            continue;
        }
        let bank = if cc_line.starts_with("cc0") {
            "cc0"
        } else {
            "cc1"
        };
        let discard = format!(
            "    if (any({bank}.{} != (float4)0)) discard;",
            lane.repeat(4)
        );
        if let Some(end) = lines[i + 2..].iter().position(|l| *l == discard) {
            let end = i + 2 + end;
            if !lines[i + 2..end].iter().any(|l| writes(l, bank, lane)) {
                alpha_discard = true;
            }
        }
    }
    if !alpha_discard {
        return unsupported();
    }
    // No guessed light/color/cutoff/encoding is inserted. Compiler DCE discards
    // unused deferred calculations; mandatory native reflection proves closure.
    program.source = lines
        .iter()
        .filter(|l| **l != product_line)
        .map(|l| format!("{l}\n"))
        .collect();
    let slice = slice::prune(program.source)?;
    program.source = slice.source;
    Ok(SpecializedFragment {
        program,
        outputs: slice.outputs,
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    fn program() -> compiler::HlslProgram {
        compiler::HlslProgram { source: concat!(
            "struct PSInput { float4 position : SV_Position; float4 tc0:TEXCOORD0; };\n",
            "Texture2D rsx_tex0 : register(t0);\nTexture2D rsx_tex10 : register(t10);\n",
            "SamplerState rsx_samp0 : register(s0);\nSamplerState rsx_samp10 : register(s10);\n",
            "cbuffer PSConstants : register(b1) { float4 fp_constants[1]; float4 fp_alpha; };\n",
            "float4 main(PSInput input) : SV_TARGET {\n    float4 h[48];\n    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n",
            "    { float4 _v = (float4)(rsx_tex10.Sample(rsx_samp10, input.tc0.xy)); h[1].xyz = _v.xyz; }\n",
            "    { float4 _v = (float4)(rsx_tex0.Sample(rsx_samp0, input.tc0.xy)); h[0].xyzw = _v.xyzw; }\n",
            "    { float4 _v = (float4)((float4)(((h[0]).wwww) <  ((fp_constants[0]).zzzz)));\n      cc0.x = _v.x;\n    }\n",
            "    if (any(cc0.xxxx != (float4)0)) discard;\n",
            "    { float4 _v = (float4)(((h[0]).xyzw) * ((h[1]).xyzw)); h[0].xyz = _v.xyz; }\n",
            "    float4 _o = h[0];\n    return (_o == _o) ? _o : (float4)0;\n}\n"
        ).into(), instructions:4,constant_slots:1,input_mask:0,current_attribute_mask:0,constant_buffer_bytes:32 }
    }
    #[test]
    fn source_cutout_and_base_sample_survive_lighting_omission() {
        let q = specialize(program()).unwrap().program;
        assert!(!q.source.contains("* ((h[1]).xyzw)"));
        assert!(q.source.contains("fp_constants[0]).zzzz"));
        assert!(q.source.contains("discard;"));
        let native =
            compiler::native_compile::lower(&q, compiler::native_compile::Stage::Fragment).unwrap();
        assert_eq!(native.textures, [0]);
        assert_eq!(native.samplers, [0]);
    }
    #[test]
    fn alpha_cutout_after_rgb_product_keeps_sample_alpha_and_source_discard() {
        let original = program();
        let product =
            "    { float4 _v = (float4)(((h[0]).xyzw) * ((h[1]).xyzw)); h[0].xyz = _v.xyz; }\n";
        let compare =
            "    { float4 _v = (float4)((float4)(((h[0]).wwww) <  ((fp_constants[0]).zzzz)));\n";
        let mut reordered = program();
        reordered.source = original
            .source
            .replace(product, "")
            .replace(compare, &format!("{product}{compare}"));
        let mut accepted = program();
        accepted.source = reordered.source.clone();
        let result = specialize(accepted).unwrap();
        assert!(!result.program.source.contains(product.trim()));
        assert!(result.program.source.contains(compare.trim()));
        assert!(
            result
                .program
                .source
                .contains("if (any(cc0.xxxx != (float4)0)) discard;")
        );
        let native = compiler::native_compile::lower(
            &result.program,
            compiler::native_compile::Stage::Fragment,
        )
        .unwrap();
        assert_eq!(native.textures, [0]);
        assert_eq!(native.samplers, [0]);
        assert_eq!(result.outputs[7], 3);

        let mut overwritten_alpha = program();
        overwritten_alpha.source = reordered.source.clone();
        overwritten_alpha.source = overwritten_alpha.source.replace(
            compare,
            &format!("    {{ float4 _v = (float4)((h[1]).xyzw); h[0].w = _v.w; }}\n{compare}"),
        );
        assert!(specialize(overwritten_alpha).is_err());
        reordered.source = reordered.source.replace(
            "    if (any(cc0.xxxx != (float4)0)) discard;",
            "    { float4 _v = (float4)((h[1]).xyzw); cc0.x = _v.x; }\n    if (any(cc0.xxxx != (float4)0)) discard;",
        );
        assert!(specialize(reordered).is_err());
    }
    #[test]
    fn unproven_alpha_or_output_refuses_arbitrary_first_sample() {
        for text in [
            program().source.replace("wwww) <", "xxxx) <"),
            program().source.replace("_o = h[0]", "_o = h[1]"),
            program().source.replace("discard;", "return 0;"),
        ] {
            let mut p = program();
            p.source = text;
            assert!(specialize(p).is_err());
        }
    }
    #[test]
    fn source_srgb_and_buffered_alpha_epilogues_stay_verbatim_and_live_uv_is_derived() {
        let original = program();
        let encoded = "    float4 _rsx_color_out = (_o == _o) ? _o : (float4)0;\n    _rsx_color_out = _rsx_srgb_output(_rsx_color_out);\n";
        let helper = "float4 _rsx_srgb_output(float4 v) { return float4(v.xyz * 2.0, v.w); }\n";
        for comparison in [
            "false",
            "_rsx_out.a < _rsx_alpha_ref",
            "_rsx_out.a >= _rsx_alpha_ref",
        ] {
            let suffix = format!(
                "{encoded}    float4 _rsx_out = _rsx_color_out;\n    const float _rsx_alpha_ref = fp_alpha.x;\n    if (!({comparison})) discard;\n    return _rsx_out;\n}}\n"
            );
            let mut p = program();
            p.source = helper.to_owned()
                + &p.source
                    .replace("    return (_o == _o) ? _o : (float4)0;\n}\n", &suffix);
            let result = specialize(p).unwrap();
            assert!(result.program.source.ends_with(&suffix));
            assert_eq!(result.outputs[0], 15);
            assert_eq!(result.outputs[7], 3);
            assert_eq!(
                result
                    .outputs
                    .iter()
                    .enumerate()
                    .filter(|(_, mask)| **mask != 0)
                    .map(|(i, _)| i)
                    .collect::<Vec<_>>(),
                [0, 7]
            );
            let native = compiler::native_compile::lower(
                &result.program,
                compiler::native_compile::Stage::Fragment,
            )
            .unwrap();
            // NEVER may remove all sampling, otherwise retain the real base.
            if comparison != "false" {
                assert_eq!(native.textures, [0]);
            }
        }
        let mut bad = original;
        bad.source = bad
            .source
            .replace("return (_o == _o) ? _o : (float4)0;", "return h[1];");
        assert!(specialize(bad).is_err());
    }
}
