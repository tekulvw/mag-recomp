use std::path::PathBuf;

fn run() -> Result<(), String> {
    let mut source = None;
    let mut out_dir = None;
    let mut stage = None;
    let mut args = std::env::args_os().skip(1);
    while let Some(argument) = args.next() {
        if argument == "--stage" {
            if stage.is_some() {
                return Err("duplicate command-line option".into());
            }
            stage = Some(
                match args
                    .next()
                    .and_then(|value| value.into_string().ok())
                    .as_deref()
                {
                    Some("compute") => mag_render_shaders::Stage::Compute,
                    Some("vertex") => mag_render_shaders::Stage::Vertex,
                    Some("fragment") => mag_render_shaders::Stage::Fragment,
                    _ => return Err("--stage requires compute, vertex, or fragment".into()),
                },
            );
            continue;
        }
        let target = if argument == "--source" {
            &mut source
        } else if argument == "--out-dir" {
            &mut out_dir
        } else {
            return Err("usage: mag-render-shaders --source INPUT.wgsl --out-dir DIRECTORY [--stage compute|vertex|fragment]".into());
        };
        if target.is_some() {
            return Err("duplicate command-line option".into());
        }
        *target = Some(PathBuf::from(args.next().ok_or("option needs a path")?));
    }
    let source = source.ok_or("--source is required")?;
    let out_dir = out_dir.ok_or("--out-dir is required")?;
    let text = std::fs::read_to_string(&source).map_err(|error| format!("read shader: {error}"))?;
    // No output is created on WGSL validation or translation failure.
    let compiled = mag_render_shaders::compile_stage(
        &text,
        stage.unwrap_or(mag_render_shaders::Stage::Compute),
    )?;
    std::fs::create_dir_all(&out_dir).map_err(|error| error.to_string())?;
    let spirv: Vec<u8> = compiled
        .spirv
        .iter()
        .flat_map(|word| word.to_le_bytes())
        .collect();
    for (name, bytes) in [
        ("shader.metal", compiled.msl.into_bytes()),
        ("shader.hlsl", compiled.hlsl.into_bytes()),
        ("shader.spv", spirv),
        (
            "manifest.json",
            serde_json::to_vec_pretty(&compiled.manifest).map_err(|error| error.to_string())?,
        ),
    ] {
        std::fs::write(out_dir.join(name), bytes).map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            std::process::ExitCode::FAILURE
        }
    }
}
