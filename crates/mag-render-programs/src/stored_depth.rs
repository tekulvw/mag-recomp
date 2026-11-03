//! Restricted Metal stored-depth test on an immutable snapshot. Pipeline owner
//! separately proves depth writes disabled and orders the source-depth copy.
use naga::*;
fn builtin_path(
    module: &Module,
    ty: Handle<Type>,
    binding: &Option<Binding>,
    depth: bool,
) -> Option<Vec<u32>> {
    if matches!(binding, Some(Binding::BuiltIn(BuiltIn::FragDepth))) && depth
        || matches!(binding, Some(Binding::BuiltIn(BuiltIn::Position { .. }))) && !depth
    {
        return Some(vec![]);
    }
    if let TypeInner::Struct { members, .. } = &module.types[ty].inner {
        for (i, m) in members.iter().enumerate() {
            if let Some(mut path) = builtin_path(module, m.ty, &m.binding, depth) {
                path.insert(0, i as u32);
                return Some(path);
            }
        }
    }
    None
}
fn kills(block: &Block) -> bool {
    block.iter().any(|s| match s {
        Statement::Kill => true,
        Statement::Block(b) => kills(b),
        Statement::If { accept, reject, .. } => kills(accept) || kills(reject),
        Statement::Switch { cases, .. } => cases.iter().any(|c| kills(&c.body)),
        Statement::Loop {
            body, continuing, ..
        } => kills(body) || kills(continuing),
        _ => false,
    })
}
pub(super) fn inject(module: &mut Module) -> Result<(), String> {
    let entry = &module.entry_points[0];
    if entry.stage != ShaderStage::Fragment
        || entry.early_depth_test.is_some()
        || entry
            .function
            .result
            .as_ref()
            .is_some_and(|r| builtin_path(module, r.ty, &r.binding, true).is_some())
        || kills(&entry.function.body)
        || module.functions.iter().any(|(_, f)| kills(&f.body))
    {
        return Err(
            "stored-depth snapshot gate requires no depth export, discard or early depth".into(),
        );
    }
    if module.global_variables.iter().any(|(_, g)| {
        matches!(
            g.binding,
            Some(ResourceBinding {
                group: 1,
                binding: 2 | 31
            })
        )
    }) {
        return Err("stored-depth hidden binding collision".into());
    }
    let position = entry
        .function
        .arguments
        .iter()
        .enumerate()
        .find_map(|(i, a)| builtin_path(module, a.ty, &a.binding, false).map(|p| (i as u32, p)));
    let span = Span::UNDEFINED;
    let vec4 = module.types.insert(
        Type {
            name: None,
            inner: TypeInner::Vector {
                size: VectorSize::Quad,
                scalar: Scalar::F32,
            },
        },
        span,
    );
    let depth = module.types.insert(
        Type {
            name: None,
            inner: TypeInner::Image {
                dim: ImageDimension::D2,
                arrayed: false,
                class: ImageClass::Depth { multi: false },
            },
        },
        span,
    );
    let image = module.global_variables.append(
        GlobalVariable {
            name: Some("mag_stored_depth".into()),
            space: AddressSpace::Handle,
            binding: Some(ResourceBinding {
                group: 1,
                binding: 31,
            }),
            ty: depth,
            init: None,
            memory_decorations: Default::default(),
        },
        span,
    );
    let bounds = module.global_variables.append(
        GlobalVariable {
            name: Some("mag_depth_bounds".into()),
            space: AddressSpace::Uniform,
            binding: Some(ResourceBinding {
                group: 1,
                binding: 2,
            }),
            ty: vec4,
            init: None,
            memory_decorations: Default::default(),
        },
        span,
    );
    let f = &mut module.entry_points[0].function;
    let (argument, path) = position.unwrap_or_else(|| {
        let index = f.arguments.len() as u32;
        f.arguments.push(FunctionArgument {
            name: Some("mag_framebuffer_position".into()),
            ty: vec4,
            binding: Some(Binding::BuiltIn(BuiltIn::Position { invariant: false })),
        });
        (index, vec![])
    });
    // Arguments, globals and literals are already in scope; do not Emit them.
    let mut pos = f
        .expressions
        .append(Expression::FunctionArgument(argument), span);
    let texture = f
        .expressions
        .append(Expression::GlobalVariable(image), span);
    let range = f
        .expressions
        .append(Expression::GlobalVariable(bounds), span);
    let zero = f
        .expressions
        .append(Expression::Literal(Literal::I32(0)), span);
    let first = f
        .expressions
        .append(Expression::Load { pointer: range }, span);
    for index in path {
        pos = f
            .expressions
            .append(Expression::AccessIndex { base: pos, index }, span);
    }
    let xy = f.expressions.append(
        Expression::Swizzle {
            size: VectorSize::Bi,
            vector: pos,
            pattern: [
                SwizzleComponent::X,
                SwizzleComponent::Y,
                SwizzleComponent::X,
                SwizzleComponent::X,
            ],
        },
        span,
    );
    let coordinate = f.expressions.append(
        Expression::As {
            expr: xy,
            kind: ScalarKind::Sint,
            convert: Some(4),
        },
        span,
    );
    let stored = f.expressions.append(
        Expression::ImageLoad {
            image: texture,
            coordinate,
            array_index: None,
            sample: None,
            level: Some(zero),
        },
        span,
    );
    let min = f.expressions.append(
        Expression::AccessIndex {
            base: first,
            index: 0,
        },
        span,
    );
    let max = f.expressions.append(
        Expression::AccessIndex {
            base: first,
            index: 1,
        },
        span,
    );
    let low = f.expressions.append(
        Expression::Binary {
            op: BinaryOperator::GreaterEqual,
            left: stored,
            right: min,
        },
        span,
    );
    let high = f.expressions.append(
        Expression::Binary {
            op: BinaryOperator::LessEqual,
            left: stored,
            right: max,
        },
        span,
    );
    let pass = f.expressions.append(
        Expression::Binary {
            op: BinaryOperator::LogicalAnd,
            left: low,
            right: high,
        },
        span,
    );
    let reject = f.expressions.append(
        Expression::Unary {
            op: UnaryOperator::LogicalNot,
            expr: pass,
        },
        span,
    );
    let mut body = Block::new();
    body.push(Statement::Emit(Range::new_from_bounds(first, reject)), span);
    body.push(
        Statement::If {
            condition: reject,
            accept: Block::from_vec(vec![Statement::Kill]),
            reject: Block::new(),
        },
        span,
    );
    body.append(&mut f.body);
    f.body = body;
    Ok(())
}
