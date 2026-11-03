//! Explicit POC position-only specialization of the controlled VP emitter.
use crate::{HlslProgram, ProgramError};
const SETUP_END: &str = "    float4 cc[2]; cc[0] = (float4)0; cc[1] = (float4)0;\n";
const EPILOGUE: &str = "    VSOutput Out;\n    float4 _p = o[0];\n    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n    Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5].xxxx;\n    Out.t0=o[7];  Out.t1=o[8];  Out.t2=o[9];  Out.t3=o[10];\n    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];\n    Out.t8=o[15]; Out.t9=o[6];\n    return Out;\n}\n";
const POSITION_END: &str = "    VSOutput Out;\n    float4 _p = o[0];\n    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n    return Out;\n}\n";
fn unsupported<T>() -> Result<T, ProgramError> {
    Err(ProgramError::Unsupported)
}
#[derive(Clone, Copy)]
struct Register {
    bank: usize, // temporary, output, or condition-code bank
    index: usize,
}
impl Register {
    fn lane(self, live: &mut [[u8; 32]; 3]) -> &mut u8 {
        &mut live[self.bank][self.index]
    }
}
fn register(text: &str) -> Result<(Register, &str), ProgramError> {
    let (bank, limit, rest) = if let Some(rest) = text.strip_prefix("r[") {
        (0, 32, rest)
    } else if let Some(rest) = text.strip_prefix("o[") {
        (1, 16, rest)
    } else if let Some(rest) = text.strip_prefix("cc[") {
        (2, 2, rest)
    } else {
        return unsupported();
    };
    let (number, rest) = rest.split_once(']').ok_or(ProgramError::Unsupported)?;
    if number.is_empty() || !number.bytes().all(|b| b.is_ascii_digit()) {
        return unsupported();
    }
    let index = number
        .parse::<usize>()
        .map_err(|_| ProgramError::Unsupported)?;
    if index >= limit {
        return unsupported();
    }
    Ok((Register { bank, index }, rest))
}
fn mask(text: &str) -> Result<u8, ProgramError> {
    let mut result = 0;
    let mut previous = 0;
    for byte in text.bytes() {
        let bit = match byte {
            b'x' => 1,
            b'y' => 2,
            b'z' => 4,
            b'w' => 8,
            _ => return unsupported(),
        };
        if bit <= previous {
            return unsupported();
        }
        previous = bit;
        result |= bit;
    }
    if result == 0 {
        return unsupported();
    }
    Ok(result)
}
struct Write {
    register: Register,
    mask: u8,
    predicate: Option<(Register, u8)>,
}
struct Block<'a> {
    text: &'a str,
    writes: Vec<Write>,
    reads: Vec<Register>,
    inputs: u16,
}
struct ExpressionReads {
    registers: Vec<Register>,
    inputs: u16,
}
fn expression(text: &str) -> Result<ExpressionReads, ProgramError> {
    let bytes = text.as_bytes();
    let mut at = 0;
    let mut depth = 0;
    let mut reads = Vec::new();
    let mut inputs = 0u16;
    while at < bytes.len() {
        let byte = bytes[at];
        if byte.is_ascii_whitespace() {
            at += 1;
            continue;
        }
        if byte.is_ascii_digit()
            || byte == b'.' && bytes.get(at + 1).is_some_and(u8::is_ascii_digit)
        {
            at += 1;
            while at < bytes.len()
                && (bytes[at].is_ascii_digit()
                    || bytes[at] == b'.'
                    || matches!(bytes[at], b'e' | b'E')
                    || matches!(bytes[at], b'+' | b'-') && matches!(bytes[at - 1], b'e' | b'E'))
            {
                at += 1;
            }
            continue;
        }
        if byte.is_ascii_alphabetic() || byte == b'_' {
            let start = at;
            at += 1;
            while at < bytes.len() && (bytes[at].is_ascii_alphanumeric() || bytes[at] == b'_') {
                at += 1;
            }
            let name = &text[start..at];
            if matches!(name, "r" | "o" | "v" | "vp_c") {
                if matches!(name, "r" | "o") {
                    let (reg, rest) = register(&text[start..])?;
                    reads.push(reg);
                    at = text.len() - rest.len();
                } else {
                    let rest = text[at..]
                        .strip_prefix('[')
                        .ok_or(ProgramError::Unsupported)?;
                    let (number, tail) = rest.split_once(']').ok_or(ProgramError::Unsupported)?;
                    if number.is_empty() || !number.bytes().all(|b| b.is_ascii_digit()) {
                        return unsupported();
                    }
                    let index = number
                        .parse::<usize>()
                        .map_err(|_| ProgramError::Unsupported)?;
                    if index >= if name == "v" { 16 } else { 512 } {
                        return unsupported();
                    }
                    if name == "v" {
                        inputs |= 1 << index;
                    }
                    at = text.len() - tail.len();
                }
            } else if name.len() <= 4
                && name.bytes().all(|b| matches!(b, b'x' | b'y' | b'z' | b'w'))
                && start > 0
                && bytes[start - 1] == b'.'
            {
                // Swizzles affect arithmetic verbatim. Register liveness is
                // conservatively all four lanes, so no cross-lane use is lost.
            } else if matches!(
                name,
                "float4"
                    | "float"
                    | "vp_mul"
                    | "vp_dot3"
                    | "vp_dot4"
                    | "abs"
                    | "min"
                    | "max"
                    | "frac"
                    | "floor"
                    | "sign"
                    | "clamp"
                    | "rsqrt"
                    | "exp2"
                    | "log2"
                    | "sin"
                    | "cos"
            ) && bytes.get(at) == Some(&b'(')
            {
            } else if matches!(name, "float4" | "float")
                && start > 0
                && bytes[start - 1] == b'('
                && bytes.get(at) == Some(&b')')
            {
                // Controlled comparison operations emit a pure HLSL cast.
            } else {
                return unsupported();
            }
            continue;
        }
        match byte {
            b'(' => depth += 1,
            b')' => {
                if depth == 0 {
                    return unsupported();
                }
                depth -= 1;
            }
            b'.' | b',' | b'+' | b'-' | b'*' | b'/' | b'<' | b'>' | b'?' | b':' => {}
            b'=' if bytes.get(at + 1) == Some(&b'=') => at += 1,
            _ => return unsupported(),
        }
        at += 1;
    }
    if depth != 0 {
        return unsupported();
    }
    Ok(ExpressionReads {
        registers: reads,
        inputs,
    })
}
fn block(text: &str) -> Result<Block<'_>, ProgramError> {
    let mut lines = text.lines();
    let rhs = lines
        .next()
        .and_then(|l| l.strip_prefix("    { float4 _v = (float4)("))
        .ok_or(ProgramError::Unsupported)?;
    let rhs = rhs
        .strip_suffix("); _v = saturate(_v);")
        .or_else(|| rhs.strip_suffix(");"))
        .ok_or(ProgramError::Unsupported)?;
    let reads = expression(rhs)?;
    let mut writes = Vec::new();
    let mut closed = false;
    for line in lines.by_ref() {
        if line == "    }" {
            closed = true;
            break;
        }
        let store = line
            .strip_prefix("        ")
            .ok_or(ProgramError::Unsupported)?;
        let (predicate, store) = if let Some(condition) = store.strip_prefix("if (") {
            let (condition, store) = condition
                .split_once(") ")
                .ok_or(ProgramError::Unsupported)?;
            let (condition_register, suffix) = register(condition)?;
            if condition_register.bank != 2 {
                return unsupported();
            }
            let (lane, comparison) = suffix
                .strip_prefix('.')
                .and_then(|s| s.split_once(' '))
                .ok_or(ProgramError::Unsupported)?;
            if lane.len() != 1
                || !matches!(
                    comparison,
                    "== 0.0" | "!= 0.0" | "< 0.0" | "<= 0.0" | "> 0.0" | ">= 0.0"
                )
            {
                return unsupported();
            }
            (Some((condition_register, mask(lane)?)), store)
        } else {
            (None, store)
        };
        let (dst, src) = store
            .split_once(" = _v.")
            .ok_or(ProgramError::Unsupported)?;
        let (reg, suffix) = register(dst)?;
        let destination = suffix.strip_prefix('.').ok_or(ProgramError::Unsupported)?;
        if src.strip_suffix(';') != Some(destination) {
            return unsupported();
        }
        writes.push(Write {
            register: reg,
            mask: mask(destination)?,
            predicate,
        });
        if writes.len() > 12 {
            return unsupported();
        }
    }
    if !closed || writes.is_empty() || lines.next().is_some() {
        return unsupported();
    }
    Ok(Block {
        text,
        writes,
        reads: reads.registers,
        inputs: reads.inputs,
    })
}
fn setup(text: &str) -> Result<(), ProgramError> {
    let prefix =
        "    float4 v[16];\n    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n";
    let tail = "    float4 r[32]; float4 o[16];\n    [unroll] for (int _i=0;_i<32;_i++) r[_i]=(float4)0;\n    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);\n    int4 a0 = (int4)0; int4 a1 = (int4)0;\n";
    let assignments = text
        .strip_prefix(prefix)
        .and_then(|s| s.strip_suffix(tail))
        .ok_or(ProgramError::Unsupported)?;
    for line in assignments.lines() {
        if !(0..16).any(|i| {
            line == format!("    v[{i}]=input.a{i};")
                || line == format!("    v[{i}]=vp_current[{i}];")
        }) {
            return unsupported();
        }
    }
    Ok(())
}
/// Explicit reduced-fidelity POC only. Input must be `vertex()`'s controlled
/// straight-line output. Retains the complete input/current/uniform contract,
/// and the exact arithmetic/viewport transform needed for position. Other
/// output computations are omitted; unsupported grammar refuses specialization.
/// This is conservative static liveness, not proof of executed instructions.
pub fn vertex_position_only_poc(program: &HlslProgram) -> Result<HlslProgram, ProgramError> {
    position_slice(program, false)
}
pub(crate) fn vertex_position_live_inputs_poc(
    program: &HlslProgram,
) -> Result<HlslProgram, ProgramError> {
    position_slice(program, true)
}
fn position_slice(
    program: &HlslProgram,
    specialize_inputs: bool,
) -> Result<HlslProgram, ProgramError> {
    if program.source.len() > 1024 * 1024 || program.instructions > 512 {
        return unsupported();
    }
    let (prefix, main) = program
        .source
        .split_once("VSOutput main(VSInput input) {\n")
        .ok_or(ProgramError::Unsupported)?;
    let (initial, body) = main
        .split_once(SETUP_END)
        .ok_or(ProgramError::Unsupported)?;
    setup(initial)?;
    let body = body
        .strip_suffix(EPILOGUE)
        .ok_or(ProgramError::Unsupported)?;
    let mut remaining = body;
    let mut blocks = Vec::new();
    while !remaining.is_empty() {
        let end = remaining.find("    }\n").ok_or(ProgramError::Unsupported)? + 6;
        blocks.push(block(&remaining[..end])?);
        if blocks.len() > 1024 {
            return unsupported();
        }
        remaining = &remaining[end..];
    }
    let mut live = [[0u8; 32]; 3];
    live[1][0] = 15;
    let mut keep = vec![false; blocks.len()];
    for (i, b) in blocks.iter().enumerate().rev() {
        if b.writes
            .iter()
            .any(|w| *w.register.lane(&mut live) & w.mask != 0)
        {
            keep[i] = true;
            // RHS is evaluated before the stores. Only unconditional stores
            // kill old lanes: a predicate may leave the previous value intact.
            // Restore predicate reads after kills too, including a CC bank
            // updated by this same block. This conservatively retains its
            // pre-store definition while the original store order is unchanged.
            for w in &b.writes {
                if w.predicate.is_none() {
                    *w.register.lane(&mut live) &= !w.mask;
                }
            }
            for r in &b.reads {
                *r.lane(&mut live) |= 15;
            }
            for w in &b.writes {
                if let Some((r, mask)) = w.predicate {
                    *r.lane(&mut live) |= mask;
                }
            }
        }
    }
    let start = prefix
        .find("struct VSOutput {\n")
        .ok_or(ProgramError::Unsupported)?;
    let end = start
        + prefix[start..]
            .find("};\n")
            .ok_or(ProgramError::Unsupported)?
        + 3;
    let mut source = String::with_capacity(program.source.len());
    source.push_str(&prefix[..start]);
    source.push_str("struct VSOutput { float4 pos:SV_Position; };\n");
    source.push_str(&prefix[end..]);
    source.push_str("VSOutput main(VSInput input) {\n");
    let live_inputs = blocks
        .iter()
        .zip(&keep)
        .filter(|(_, keep)| **keep)
        .fold(0u16, |mask, (block, _)| mask | block.inputs);
    if u32::from(live_inputs) & !program.input_mask != 0 {
        return unsupported();
    }
    if specialize_inputs {
        for line in initial.split_inclusive('\n') {
            let dead_assignment = (0..16).any(|i| {
                live_inputs & (1 << i) == 0
                    && (line == format!("    v[{i}]=input.a{i};\n")
                        || line == format!("    v[{i}]=vp_current[{i}];\n"))
            });
            if !dead_assignment {
                source.push_str(line);
            }
        }
    } else {
        source.push_str(initial);
    }
    source.push_str(SETUP_END);
    for (b, keep) in blocks.iter().zip(keep) {
        if keep {
            source.push_str(b.text);
        }
    }
    source.push_str(POSITION_END);
    Ok(HlslProgram {
        source,
        instructions: program.instructions,
        constant_slots: program.constant_slots,
        input_mask: if specialize_inputs {
            u32::from(live_inputs)
        } else {
            program.input_mask
        },
        current_attribute_mask: program.current_attribute_mask,
        constant_buffer_bytes: program.constant_buffer_bytes,
    })
}
