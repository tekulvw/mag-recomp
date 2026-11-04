//! Pure lane-dependency pruning of the controlled straight-line FP emitter.
//! All blocks are parsed before pruning; unsupported syntax never disappears
//! merely because it might be dead. Predicated stores/loops/exports refuse.
use super::{CompileError, unsupported};
type Dependencies = [u8; 112];
#[derive(Clone)]
struct Value {
    width: usize,
    lanes: [Dependencies; 4],
}
impl Value {
    fn external(width: usize) -> Self {
        Self {
            width,
            lanes: [[0; 112]; 4],
        }
    }
    fn broadcast(mut self, width: usize) -> Self {
        if self.width == 1 {
            for i in 1..width {
                self.lanes[i] = self.lanes[0];
            }
        }
        self.width = width;
        self
    }
    fn all(&self) -> Dependencies {
        let mut out = [0; 112];
        for lane in &self.lanes[..self.width] {
            merge(&mut out, lane);
        }
        out
    }
}
fn merge(a: &mut Dependencies, b: &Dependencies) {
    for (a, b) in a.iter_mut().zip(b) {
        *a |= *b;
    }
}
fn reg(name: &str, index: usize) -> Result<usize, CompileError> {
    match name {
        "h" if index < 48 => Ok(index),
        "r" if index < 48 => Ok(48 + index),
        "cc0" => Ok(96),
        "cc1" => Ok(97),
        _ => unsupported(),
    }
}
fn tokenize(text: &str) -> Result<Vec<&str>, CompileError> {
    let b = text.as_bytes();
    let mut at = 0;
    let mut out = Vec::new();
    while at < b.len() {
        if b[at].is_ascii_whitespace() {
            at += 1;
            continue;
        }
        let start = at;
        if b[at].is_ascii_alphabetic() || b[at] == b'_' {
            at += 1;
            while at < b.len() && (b[at].is_ascii_alphanumeric() || b[at] == b'_') {
                at += 1;
            }
        } else if b[at].is_ascii_digit()
            || b[at] == b'.' && b.get(at + 1).is_some_and(u8::is_ascii_digit)
        {
            at += 1;
            while at < b.len()
                && (b[at].is_ascii_digit()
                    || matches!(b[at], b'.' | b'e' | b'E')
                    || matches!(b[at], b'+' | b'-') && matches!(b[at - 1], b'e' | b'E'))
            {
                at += 1;
            }
            if b.get(at) == Some(&b'u') {
                at += 1;
            }
        } else if b"()[],.+-*/<>!=?:".contains(&b[at]) {
            at += 1;
            if at < b.len() && b[at] == b'=' && matches!(b[start], b'<' | b'>' | b'!' | b'=') {
                at += 1;
            }
        } else {
            return unsupported();
        }
        out.push(&text[start..at]);
    }
    Ok(out)
}
struct Parser<'a> {
    tokens: Vec<&'a str>,
    at: usize,
}
impl<'a> Parser<'a> {
    fn peek(&self) -> Option<&'a str> {
        self.tokens.get(self.at).copied()
    }
    fn pop(&mut self) -> Result<&'a str, CompileError> {
        let t = self.peek().ok_or(CompileError::Decode(
            super::compiler::ProgramError::Unsupported,
        ))?;
        self.at += 1;
        Ok(t)
    }
    fn eat(&mut self, t: &str) -> Result<(), CompileError> {
        if self.pop()? == t {
            Ok(())
        } else {
            unsupported()
        }
    }
    fn expression(&mut self, min: u8) -> Result<Value, CompileError> {
        let mut a = self.unary()?;
        while let Some(op) = self.peek() {
            let prec = match op {
                "==" | "!=" | "<" | ">" | "<=" | ">=" => 1,
                "+" | "-" => 2,
                "*" | "/" => 3,
                _ => break,
            };
            if prec < min {
                break;
            }
            self.at += 1;
            let b = self.expression(prec + 1)?;
            a = combine(a, b)?;
        }
        Ok(a)
    }
    fn unary(&mut self) -> Result<Value, CompileError> {
        if matches!(self.peek(), Some("+" | "-" | "!")) {
            self.at += 1;
            return self.unary();
        }
        let mut value = if self.peek() == Some("(") {
            self.at += 1;
            if matches!(self.peek(), Some("float4" | "float"))
                && self.tokens.get(self.at + 1) == Some(&")")
            {
                let width = if self.pop()? == "float4" { 4 } else { 1 };
                self.eat(")")?;
                let value = self.unary()?;
                if value.width != 1 && value.width != width {
                    return unsupported();
                }
                value.broadcast(width)
            } else {
                let v = self.expression(0)?;
                self.eat(")")?;
                v
            }
        } else {
            let token = self.pop()?;
            if token
                .bytes()
                .next()
                .is_some_and(|b| b.is_ascii_digit() || b == b'.')
            {
                Value::external(1)
            } else if matches!(token, "h" | "r" | "cc0" | "cc1") {
                let index = if matches!(token, "h" | "r") {
                    self.eat("[")?;
                    let n = self.pop()?.parse().map_err(|_| {
                        CompileError::Decode(super::compiler::ProgramError::Unsupported)
                    })?;
                    self.eat("]")?;
                    reg(token, n)?
                } else {
                    reg(token, 0)?
                };
                let mut v = Value::external(4);
                for lane in 0..4 {
                    v.lanes[lane][index] = 1 << lane;
                }
                v
            } else if token == "input" {
                self.eat(".")?;
                let field = self.pop()?;
                if !matches!(
                    field,
                    "position"
                        | "col0"
                        | "col1"
                        | "fog"
                        | "tc0"
                        | "tc1"
                        | "tc2"
                        | "tc3"
                        | "tc4"
                        | "tc5"
                        | "tc6"
                        | "tc7"
                        | "tc8"
                        | "tc9"
                ) {
                    return unsupported();
                }
                let input_index =
                    match field {
                        "position" => 98,
                        "col0" => 99,
                        "col1" => 100,
                        "fog" => 101,
                        _ => {
                            102 + field.strip_prefix("tc").unwrap().parse::<usize>().map_err(
                                |_| {
                                    CompileError::Decode(super::compiler::ProgramError::Unsupported)
                                },
                            )?
                        }
                    };
                let mut value = Value::external(4);
                for lane in 0..4 {
                    value.lanes[lane][input_index] = 1 << lane;
                }
                value
            } else if token == "fp_constants" {
                self.eat("[")?;
                let n = self.pop()?.parse::<usize>().map_err(|_| {
                    CompileError::Decode(super::compiler::ProgramError::Unsupported)
                })?;
                if n >= 2048 {
                    return unsupported();
                }
                self.eat("]")?;
                Value::external(4)
            } else if token == "fp_alpha" || token.starts_with("rsx_samp") {
                Value::external(4)
            } else if token.starts_with("rsx_tex") {
                self.eat(".")?;
                let method = self.pop()?;
                if method != "Sample" {
                    return unsupported();
                }
                let args = self.args()?;
                let mut deps = [0; 112];
                for a in args {
                    merge(&mut deps, &a.all());
                }
                Value {
                    width: 4,
                    lanes: [deps; 4],
                }
            } else if self.peek() == Some("(") {
                let args = self.args()?;
                match token {
                    "float2" | "float3" | "float4" => {
                        let width = match token {
                            "float2" => 2,
                            "float3" => 3,
                            _ => 4,
                        };
                        if args.len() == 1 && args[0].width == 1 {
                            args.into_iter().next().unwrap().broadcast(width)
                        } else {
                            // HLSL vector constructors concatenate components,
                            // as in the source AA WPOS transform vec2+vec2.
                            // Do not truncate, pad or invent a missing lane.
                            if args.iter().map(|a| a.width).sum::<usize>() != width {
                                return unsupported();
                            }
                            let mut out = Value::external(width);
                            let mut at = 0;
                            for a in args {
                                for lane in &a.lanes[..a.width] {
                                    out.lanes[at] = *lane;
                                    at += 1;
                                }
                            }
                            out
                        }
                    }
                    // The controlled gamma helper maps each output lane from
                    // that same input lane. Keep its source call/body verbatim;
                    // the uint mask is not a temporary-register dependency.
                    "_rsx_gamma_input" => {
                        if args.len() != 2
                            || args[0].width != 4
                            || args[1].width != 1
                            || args[1].all().iter().any(|mask| *mask != 0)
                        {
                            return unsupported();
                        }
                        args.into_iter().next().unwrap()
                    }
                    "_rsx_inv_w" | "dot" | "any" => {
                        let mut deps = [0; 112];
                        for a in args {
                            merge(&mut deps, &a.all());
                        }
                        Value {
                            width: 1,
                            lanes: [deps; 4],
                        }
                    }
                    "abs" | "min" | "max" | "clamp" | "saturate" | "log2" | "exp2" | "rsqrt"
                    | "sqrt" | "floor" | "frac" | "pow" => {
                        let mut it = args.into_iter();
                        let Some(mut v) = it.next() else {
                            return unsupported();
                        };
                        for a in it {
                            v = combine(v, a)?;
                        }
                        v
                    }
                    _ => return unsupported(),
                }
            } else {
                return unsupported();
            }
        };
        while self.peek() == Some(".") {
            self.at += 1;
            let swizzle = self.pop()?;
            if swizzle.is_empty() || swizzle.len() > 4 {
                return unsupported();
            }
            let mut out = Value::external(swizzle.len());
            for (at, b) in swizzle.bytes().enumerate() {
                let lane = match b {
                    b'x' => 0,
                    b'y' => 1,
                    b'z' => 2,
                    b'w' => 3,
                    _ => return unsupported(),
                };
                if lane >= value.width {
                    return unsupported();
                }
                out.lanes[at] = value.lanes[lane];
            }
            value = out;
        }
        Ok(value)
    }
    fn args(&mut self) -> Result<Vec<Value>, CompileError> {
        self.eat("(")?;
        let mut args = Vec::new();
        loop {
            args.push(self.expression(0)?);
            match self.pop()? {
                "," => {}
                ")" => break,
                _ => return unsupported(),
            }
        }
        Ok(args)
    }
}
fn combine(a: Value, b: Value) -> Result<Value, CompileError> {
    let width = a.width.max(b.width);
    if a.width != b.width && a.width != 1 && b.width != 1 {
        return unsupported();
    }
    let mut a = a.broadcast(width);
    let b = b.broadcast(width);
    for i in 0..width {
        merge(&mut a.lanes[i], &b.lanes[i]);
    }
    Ok(a)
}
fn expression(s: &str) -> Result<Value, CompileError> {
    let mut p = Parser {
        tokens: tokenize(s)?,
        at: 0,
    };
    let out = p.expression(0)?;
    if p.peek().is_some() {
        return unsupported();
    }
    Ok(out)
}
struct Write {
    register: usize,
    lane: usize,
    rhs: Dependencies,
}
struct Block<'a> {
    text: &'a str,
    writes: Vec<Write>,
    discard: Option<Dependencies>,
}
fn block(text: &str) -> Result<Block<'_>, CompileError> {
    if let Some(condition) = text
        .trim()
        .strip_prefix("if (any(")
        .and_then(|s| s.strip_suffix(" != (float4)0)) discard;"))
    {
        return Ok(Block {
            text,
            writes: vec![],
            discard: Some(expression(condition)?.all()),
        });
    }
    let Some(body) = text
        .trim()
        .strip_prefix("{ float4 _v = ")
        .and_then(|s| s.strip_suffix('}'))
    else {
        return unsupported();
    };
    let Some((rhs, stores)) = body.split_once(';') else {
        return unsupported();
    };
    let value = expression(rhs)?;
    let mut writes = Vec::new();
    for store in stores.split(';').map(str::trim).filter(|s| !s.is_empty()) {
        if store == "_v = saturate(_v)" {
            continue;
        }
        let Some((target, rhs)) = store.split_once(" = _v.") else {
            return unsupported();
        };
        let Some((target, mask)) = target.rsplit_once('.') else {
            return unsupported();
        };
        let index = if matches!(target, "cc0" | "cc1") {
            reg(target, 0)?
        } else {
            let Some((name, n)) = target.split_once('[') else {
                return unsupported();
            };
            let Some(n) = n.strip_suffix(']') else {
                return unsupported();
            };
            reg(
                name,
                n.parse().map_err(|_| {
                    CompileError::Decode(super::compiler::ProgramError::Unsupported)
                })?,
            )?
        };
        if mask.len() != rhs.len() || mask.len() > 4 {
            return unsupported();
        }
        let mut written = 0;
        for (lane, source) in mask.bytes().zip(rhs.bytes()) {
            let l = match lane {
                b'x' => 0,
                b'y' => 1,
                b'z' => 2,
                b'w' => 3,
                _ => return unsupported(),
            };
            let r = match source {
                b'x' => 0,
                b'y' => 1,
                b'z' => 2,
                b'w' => 3,
                _ => return unsupported(),
            };
            if written & (1 << l) != 0 || r >= value.width {
                return unsupported();
            }
            written |= 1 << l;
            writes.push(Write {
                register: index,
                lane: l,
                rhs: value.lanes[r],
            });
        }
    }
    if writes.is_empty() {
        return unsupported();
    }
    Ok(Block {
        text,
        writes,
        discard: None,
    })
}
pub(super) struct Slice {
    pub source: String,
    pub outputs: [u8; 16],
}
pub(super) fn prune(source: String) -> Result<Slice, CompileError> {
    const SETUP: &str = "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n";
    let Some((prefix, body)) = source.split_once(SETUP) else {
        return unsupported();
    };
    let Some((body, end)) = body.split_once("    float4 _o = ") else {
        return unsupported();
    };
    let Some(base) = end.split(';').next() else {
        return unsupported();
    };
    let index = if let Some((name, n)) = base.split_once('[') {
        reg(
            name,
            n.trim_end_matches(']')
                .parse()
                .map_err(|_| CompileError::Decode(super::compiler::ProgramError::Unsupported))?,
        )?
    } else {
        return unsupported();
    };
    let mut blocks = Vec::new();
    let mut at = 0;
    while at < body.len() {
        let rest = &body[at..];
        if rest.trim().is_empty() {
            break;
        }
        let count = if rest.starts_with("    { ") {
            rest.find('}').ok_or(CompileError::Decode(
                super::compiler::ProgramError::Unsupported,
            ))? + 1
        } else {
            rest.find('\n').unwrap_or(rest.len())
        };
        let count = count + usize::from(rest.as_bytes().get(count) == Some(&b'\n'));
        let text = &rest[..count];
        blocks.push(block(text)?);
        at += count;
    }
    let mut live = [0u8; 112];
    live[index] = 15;
    let mut keep = vec![false; blocks.len()];
    for (i, b) in blocks.iter().enumerate().rev() {
        if let Some(reads) = &b.discard {
            merge(&mut live, reads);
            keep[i] = true;
            continue;
        }
        let mut reads = [0u8; 112];
        for w in &b.writes {
            if live[w.register] & (1 << w.lane) != 0 {
                keep[i] = true;
                merge(&mut reads, &w.rhs);
            }
        }
        if keep[i] {
            for w in &b.writes {
                live[w.register] &= !(1 << w.lane);
            }
            merge(&mut live, &reads);
        }
    }
    // Source initializes temporary/CC banks explicitly; retaining these starts
    // is not a new value/default. Keep both initialization and epilogue verbatim.
    let mut outputs = [0u8; 16];
    outputs[0] = 15;
    outputs[1] = live[99];
    outputs[2] = live[100];
    outputs[5] = u8::from(live[101] != 0);
    for tc in 0..10 {
        outputs[if tc == 9 { 6 } else { 7 + tc }] = live[102 + tc];
    }
    let mut out = format!("{prefix}{SETUP}");
    for (b, k) in blocks.iter().zip(keep) {
        if k {
            out.push_str(b.text);
        }
    }
    out.push_str("    float4 _o = ");
    out.push_str(end);
    Ok(Slice {
        source: out,
        outputs,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn source(body: &str) -> String {
        format!(
            "prefix\n    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n{body}    float4 _o = h[0];\n    return (_o == _o) ? _o : (float4)0;\n}}\n"
        )
    }
    #[test]
    fn lane_overwrite_self_read_and_scalar_broadcast_keep_actual_dependencies() {
        let text = source(concat!(
            "    { float4 _v = (float4)((r[1]).xyzw); h[0].xyzw = _v.xyzw; }\n",
            "    { float4 _v = (float4)((r[2]).xyzw); h[1].xyzw = _v.xyzw; }\n",
            "    { float4 _v = (float4)((h[1]).xxxx); h[1].y = _v.y; }\n",
            "    { float4 _v = (float4)(((h[0]).xyzw) / ((h[1]).yyyy).x); h[0].xyz = _v.xyz; }\n",
            "    { float4 _v = (float4)((r[7]).xyzw); h[4].xyzw = _v.xyzw; }\n"
        ));
        let out = prune(text).unwrap().source;
        assert!(out.contains("r[1]"));
        assert!(out.contains("r[2]"));
        assert!(!out.contains("r[7]"));
        assert!(out.contains("h[1].y = _v.y"));
    }
    #[test]
    fn dead_unknown_control_or_conditional_store_is_not_silently_removed() {
        for dead in [
            "    { float4 _v = (float4)(unknown((r[1]).xyzw)); h[4].x = _v.x; }\n",
            "    { float4 _v = (float4)((r[1]).xyzw); if (cc0.x != 0.0) h[4].x = _v.x; }\n",
            "    while (cc0.x != 0.0) { h[4].x = 1; }\n",
        ] {
            assert!(prune(source(dead)).is_err());
        }
    }
    #[test]
    fn all_discard_roots_and_prewrite_rhs_are_kept() {
        let text = source(concat!(
            "    { float4 _v = (float4)((h[0]).wwww); h[0].w = _v.w; cc0.x = _v.x; }\n",
            "    if (any(cc0.xxxx != (float4)0)) discard;\n",
            "    { float4 _v = (float4)((r[2]).xyzw); h[0].xyzw = _v.xyzw; }\n"
        ));
        let out = prune(text).unwrap().source;
        assert!(out.contains("cc0.x = _v.x"));
        assert!(out.contains("discard;"));
        assert!(out.contains("r[2]"));
    }
}

#[cfg(test)]
mod gamma_tests {
    use super::*;
    #[test]
    fn source_gamma_helper_keeps_component_dependencies_and_actual_unsigned_mask() {
        let value = expression("_rsx_gamma_input((h[3]).xyzw, 7u)").unwrap();
        assert_eq!(value.width, 4);
        for lane in 0..4 {
            assert_eq!(value.lanes[lane][3], 1 << lane);
            assert_eq!(
                value.lanes[lane].iter().filter(|mask| **mask != 0).count(),
                1
            );
        }
        // An unknown helper or register-dependent mask is not silently erased.
        assert!(expression("_unknown_gamma((h[3]).xyzw, 7u)").is_err());
        assert!(expression("_rsx_gamma_input((h[3]).xyzw, (h[1]).x)").is_err());
    }
    #[test]
    fn kept_fragment_input_lanes_define_vertex_outputs_without_dead_light_inputs() {
        let text = concat!(
            "prefix\n    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n",
            "    { float4 _v = (float4)((input.tc0).xyxy); h[0].xy = _v.xy; }\n",
            "    { float4 _v = (float4)((input.tc4).wwww); h[0].zw = _v.zw; }\n",
            "    { float4 _v = (float4)((input.tc5).xyzw); h[1].xyzw = _v.xyzw; }\n",
            "    float4 _o = h[0];\n    return (_o == _o) ? _o : (float4)0;\n}\n"
        )
        .to_owned();
        let result = prune(text).unwrap();
        let mut expected = [0; 16];
        expected[0] = 15;
        expected[7] = 3;
        expected[11] = 8;
        assert_eq!(result.outputs, expected);
        assert!(!result.source.contains("input.tc5"));
        for (field, out) in [("col0", 1), ("col1", 2), ("fog", 5), ("tc9", 6)] {
            let text = format!(
                "prefix\n    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n    {{ float4 _v = (float4)((input.{field}).xyzw); h[0].xyzw = _v.xyzw; }}\n    float4 _o = h[0];\n    return (_o == _o) ? _o : (float4)0;\n}}\n"
            );
            let result = prune(text).unwrap();
            assert_eq!(result.outputs[out], if field == "fog" { 1 } else { 15 });
        }
    }
}

#[cfg(test)]
mod aa_constructor_tests {
    use super::*;
    #[test]
    fn source_aa_wpos_constructor_concatenates_exact_lanes_without_padding() {
        let value = expression("float4((h[3]).xy * float2(0.5, 1.0), (r[2]).zw)").unwrap();
        assert_eq!(value.width, 4);
        assert_eq!(value.lanes[0][3], 1);
        assert_eq!(value.lanes[1][3], 2);
        assert_eq!(value.lanes[2][50], 4);
        assert_eq!(value.lanes[3][50], 8);
        for lane in value.lanes {
            assert_eq!(lane.iter().filter(|m| **m != 0).count(), 1);
        }
        assert!(expression("float4((h[3]).xy)").is_err());
        assert!(expression("float4((h[3]).xyz, (r[2]).zw)").is_err());
        assert!(expression("unknown4((h[3]).xy, (r[2]).zw)").is_err());
    }
}
