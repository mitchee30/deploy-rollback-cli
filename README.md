# DeployGuard CLI (deploy-rollback-cli)

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-blue)
![License](https://img.shields.io/badge/License-MIT-yellow)

A lightweight, fault-tolerant Command-Line Interface (CLI) built with modern C++ for automated software deployment and intelligent state rollback. 

This tool is designed to act as a reliable state-machine for CI/CD pipelines, ensuring that any deployment failure triggers an immediate and clean rollback to a safe state, minimizing system downtime.

## ✨ Key Features

* **Automated Deployment Execution:** Seamlessly triggers and monitors external deployment scripts or binaries.
* **Intelligent Rollback Mechanism:** Real-time capturing of process exit codes. Automatically halts and initiates recovery scripts if a non-zero exit status is detected.
* **Process Sub-system Monitoring:** Built to directly interface with the OS environment using standard C/C++ libraries.
* **Extensible Scripting Foundation:** Decouples core logic from execution logic via flexible shell scripting (`deploy.sh`, `rollback.sh`).

## 📁 Repository Structure

```text
.
├── CMakeLists.txt        # CMake build configuration
├── include/
│   └── deploy-cli.h      # Core headers and definitions
├── scripts/
│   ├── build.sh          # Automated compilation script
│   ├── deploy.sh         # Mock deployment execution script
│   └── rollback.sh       # Mock system recovery script
└── src/
    └── main.cpp          # CLI entry point and state-machine logic
```

## 🚀 Getting Started

### Prerequisites

* A C++17 compatible compiler (GCC, Clang, or MSVC)
* CMake (3.10 or higher)

### Build Instructions

You can quickly build the project using the provided shell script:

```bash
chmod +x scripts/build.sh
./scripts/build.sh
```

Alternatively, use standard CMake commands:

```bash
mkdir build && cd build
cmake ..
make
```

### Usage

Run the CLI tool by specifying the target deployment path:

```bash
./deploy-cli --path /var/www/target_app
```

Expected Workflow (Simulated):
1. The CLI parses the arguments and invokes `scripts/deploy.sh`.
2. If `deploy.sh` encounters an error (e.g., exit 1), the CLI intercepts the error.
3. The CLI immediately triggers `scripts/rollback.sh` to restore the system state.
4. Logs are outputted to the standard output/error stream for pipeline monitoring.

## 🛠️ Future Roadmap

* [x] Replace `std::system` with POSIX `popen` for real-time stdout/stderr streaming.
* [ ] Implement a persistent logging system to write deployment histories to `.log` files.
* [ ] Add JSON configuration parsing for complex deployment workflows.

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.
