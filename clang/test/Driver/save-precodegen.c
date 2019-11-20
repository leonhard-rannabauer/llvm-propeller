// RUN: %clang -### -target x86_64-unknown-linux --save-precodegen -c -o bindir/save-precodegen.o %s 2>&1 | FileCheck -check-prefix=PRECODEGEN %s
// RUN: %clang -### -target x86_64-unknown-linux --save-precodegen  -flto=thin -fuse-ld=lld %s 2>&1 | FileCheck -check-prefix=PRECODEGEN-LTO %s

// Check for suffix: "precodegen.o" and that it writes to the output dir.
// PRECODEGEN: bindir/save-precodegen.precodegen.o
// Check if disable-llvm-passes is active when compiling from the precodegen ir.
// PRECODEGEN: "-disable-llvm-passes" {{.*}} "-o" "bindir/save-precodegen.o" "-x" "ir" "bindir/save-precodegen.precodegen.o"

// With LTO, the linker must save the precodegen file.
// PRECOEGEN-LTO-NOT: bindir/save-precodegen.precodegen.o
// Check if the linker gets the options.
// PRECODEGEN-LTO:ld.lld{{.*}} "--save-precodegen"
