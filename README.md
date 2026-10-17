# Risk Compiler (`riskc`)

`riskc` is a single-pass, sequential compiler for Risk, a statically typed programming language that lowers to C99 and delegates final machine code generation to GCC.

## Repository Structure

- `src/`: Contains all compiler source files, including the lexer, parser, type checker, code generator, and compilation driver.
- `CMakeLists.txt`: The primary CMake configuration file for building the project.
- `ARCHITECTURE.md`: Developer guides and internal architecture references.
- `examples/`: Sample Risk programs and feature demonstrations.
- `manual/`: The language manual and syntax reference.
- `stdlib/`: Standard library modules written for Risk.

## Building the Project

To build the Risk compiler from source, ensure you have the following prerequisites installed:
- A C++ compiler supporting C++17 or later
- CMake 3.10 or higher
- GCC (required for final code generation and linking)

### Build Instructions

1. Navigate to the root directory of the project.
2. Create a build directory and run CMake:
   ```bash
   mkdir build
   cd build
   cmake ..```

3. Compile the source code:
```bash
make
```

This will generate the `riskc` executable inside your build directory.

## Running the Language

To compile a Risk source file into an executable, invoke the compiler with the source path and an output flag:

```bash
./riskc path/to/source.risk -o output_binary

```

Once compiled, you can run the resulting executable directly:

```bash
./output_binary

```

### Example Usage

Create a file named `main.risk` with the following content:

```risk
func main() -> i32 {
    println("Hello, Risk!");
    return 0;
}

```

Compile and run the program:

```bash
./riskc main.risk -o app
./app

```

Expected output:

```text
Hello, Risk!
```
