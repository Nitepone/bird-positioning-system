use bsp_server::config::ServerConfig;
use clap::Parser;
use std::path::PathBuf;

#[derive(Parser)]
#[command(version, about = "bsp server: bird call identification and location")]
struct Args {
    /// TOML configuration file (defaults are used when omitted).
    #[arg(short, long)]
    config: Option<PathBuf>,
    /// Print the effective configuration as TOML and exit.
    #[arg(long)]
    print_config: bool,
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info,ort=warn".into()),
        )
        .init();
    let args = Args::parse();
    let cfg = match &args.config {
        Some(path) => ServerConfig::load(path)?,
        None => ServerConfig::default(),
    };
    if args.print_config {
        print!("{}", toml::to_string_pretty(&cfg)?);
        return Ok(());
    }
    bsp_server::run(cfg).await
}
