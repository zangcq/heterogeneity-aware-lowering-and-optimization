//===- driver.cc ----------------------------------------------------------===//
//
// Copyright (C) 2019-2020 Alibaba Group Holding Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <fstream>
#include <set>
#include <string>

#include "halo/lib/framework/common.h"
#include "halo/lib/ir/ir_builder.h"
#include "halo/lib/parser/parser.h"
#include "halo/lib/pass/pass_manager.h"
#include "halo/lib/target/cpu/arm/binary/arm_llvmir_codegen.h"
#include "halo/lib/target/cpu/riscv/binary/riscv_llvmir_codegen.h"
#include "halo/lib/target/cpu/x86/binary/x86_llvmir_codegen.h"
#include "halo/lib/target/generic_cxx/generic_cxx_codegen.h"
#include "halo/lib/target/generic_llvmir/generic_llvmir_codegen.h"
#include "halo/lib/target/triton/triton_config_writer.h"
#include "halo/lib/transforms/caffeextension_legalizer.h"
#include "halo/lib/transforms/dce.h"
#include "halo/lib/transforms/device_placement.h"
#include "halo/lib/transforms/fusion.h"
#include "halo/lib/transforms/input_legalizer.h"
#include "halo/lib/transforms/input_rewriter.h"
#include "halo/lib/transforms/inst_simplify.h"
#include "halo/lib/transforms/onnxextension_legalizer.h"
#include "halo/lib/transforms/output_rewriter.h"
#include "halo/lib/transforms/reorder_channel.h"
#include "halo/lib/transforms/splitting.h"
#include "halo/lib/transforms/tfextension_legalizer.h"
#include "halo/lib/transforms/type_legalizer.h"
#include "halo/version.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"

using namespace halo;

static llvm::cl::list<std::string> ModelFiles(
    llvm::cl::Positional, llvm::cl::desc("model file name."),
    llvm::cl::OneOrMore);
static llvm::cl::opt<std::string> Target(
    "target", llvm::cl::desc("target triple"),
    llvm::cl::init("x86_64-unknown-linux"));
static llvm::cl::opt<std::string> Processor("processor",
                                            llvm::cl::desc("processor name"),
                                            llvm::cl::init("native"));
static llvm::cl::opt<std::string> OutputFile(
    "o", llvm::cl::desc("output file name."), llvm::cl::Required);
static llvm::cl::opt<Parser::Format> ModelFormat(
    "x",
    llvm::cl::desc(
        "format of the following input model files. Permissible formats "
        "include: TENSORFLOW CAFFE ONNX MXNET. If unspecified, the format is "
        "guessed base on file's extension."),
    llvm::cl::init(Parser::Format::INVALID));
static llvm::cl::opt<bool> PrintAll(
    "print-all", llvm::cl::desc("print intermediates of all passes"),
    llvm::cl::init(false));
static llvm::cl::opt<bool> EmitLLVMIR("emit-llvm",
                                      llvm::cl::desc("output the LLVM IR code"),
                                      llvm::cl::init(false));
static llvm::cl::opt<std::string> EntryFunctionName(
    "entry-func-name", llvm::cl::desc("name of entry function"),
    llvm::cl::init(""));

static llvm::cl::opt<std::string> ModuleName("module-name",
                                             llvm::cl::desc("name of module"),
                                             llvm::cl::init("halo_module"));

static llvm::cl::opt<ReorderChannel::ChannelOrder> ReorderChannelLayout(
    llvm::cl::values(clEnumValN(ReorderChannel::ChannelOrder::None, "none",
                                "No reordering"),
                     clEnumValN(ReorderChannel::ChannelOrder::ChannelFirst,
                                "channel-first", "Reorder to channel first"),
                     clEnumValN(ReorderChannel::ChannelOrder::ChannelLast,
                                "channel-last", "Reorder to channel last")),
    "reorder-data-layout", llvm::cl::desc("Reorder the data layout"),
    llvm::cl::init(ReorderChannel::ChannelOrder::None));

static llvm::cl::opt<bool> RemoveInputTranspose(
    "remove-input-transpose", llvm::cl::desc("Remove the transpose for inputs"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> RemoveOutputTranspose(
    "remove-output-transpose",
    llvm::cl::desc("Remove the transpose for outputs"), llvm::cl::init(false));

static llvm::cl::list<std::string> InputsShape(
    "input-shape",
    llvm::cl::desc("Specify input names like -input-shape=foo:1x3x100x100 "
                   "-input-shape=bar:-1x3x200x200"));

static llvm::cl::opt<bool> SeparateConstants(
    "separate-constants",
    llvm::cl::desc("Generate separate file for constants"),
    llvm::cl::init(true));
static llvm::cl::opt<bool> DisableBroadcasting(
    "disable-broadcasting", llvm::cl::desc("disable broadcasting of constants"),
    llvm::cl::init(false));
static llvm::cl::opt<bool> EmitCodeOnly(
    "code-only", llvm::cl::desc("Generate the code only"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> RISCVOpt(
    "riscv-opt", llvm::cl::desc("Enable optimizations for RISC-V only"),
    llvm::cl::init(false));

static llvm::cl::opt<int> Batch(
    "batch-size",
    llvm::cl::desc("Specify batch size if the first dim of input is negative"),
    llvm::cl::init(1));

static llvm::cl::opt<bool> EnableBF16("enable-bf16",
                                      llvm::cl::desc("Enable BF16"),
                                      llvm::cl::init(false));

static llvm::cl::opt<bool> DisableCodeFormat(
    "disable-code-format",
    llvm::cl::desc("Disable formatting the generated C/C++ code"),
    llvm::cl::init(false));

static llvm::cl::opt<CodeGen::ExecMode> ExecMode(
    llvm::cl::values(clEnumValN(CodeGen::ExecMode::Compile, "compile",
                                "Compilation-Execution Model"),
                     clEnumValN(CodeGen::ExecMode::Interpret, "interpret",
                                "Interpreter Model")),
    "exec-mode", llvm::cl::desc("Execution model of emitted code"),
    llvm::cl::init(CodeGen::ExecMode::Compile));

static llvm::cl::opt<bool> EmitDataAsC(
    "emit-data-as-c", llvm::cl::desc("Emit Constants as C/C++ code"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> PrintMemStats(
    "print-mem-stats", llvm::cl::desc("Print Memory Usage Stats"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> EmitValueReset(
    "emit-value-reset",
    llvm::cl::desc("Emit code to reset value life cycle ends"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> EmitValueIDAsInt(
    "emit-value-id-as-int",
    llvm::cl::desc("Emit value id as integer. (default is string"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> SplitFunction(
    "fiss-function",
    llvm::cl::desc("Split the function into multiple subfunctions"),
    llvm::cl::init(false));

static llvm::cl::opt<CodeGen::API> Api(
    llvm::cl::values(clEnumValN(CodeGen::API::HALO_RT, "halo_rt",
                                "Using Halo Runtime Library"),
                     clEnumValN(CodeGen::API::ODLA_05, "odla_05",
                                "Using ODLA 0.5")),
    "api", llvm::cl::desc("APIs used in emitted code"),
    llvm::cl::init(CodeGen::API::ODLA_05));

static llvm::cl::opt<bool> EmitInferenceFunctionSignature(
    "emit-inference-func-sig",
    llvm::cl::desc("Emit fuction with a universal signature in c/c++ codegen"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> EmitTritonConfig(
    "emit-triton-config",
    llvm::cl::desc("Emit triton inference server config file"),
    llvm::cl::init(false));

static llvm::cl::opt<std::string> TritonConfigFile(
    "triton-config-file", llvm::cl::desc("Triton inference server config file"),
    llvm::cl::init("config.pbtxt"));

static llvm::cl::list<std::string> Inputs(
    "inputs",
    llvm::cl::desc("Specify input names like -inputs=foo -inputs=bar"));

static llvm::cl::list<std::string> Outputs(
    "outputs",
    llvm::cl::desc("Specify output names like -outputs=foo, -outputs=bar:0"));

#undef HALO_FUSION_OPTIONS
#define HALO_FUSION_CMD_OPTIONS_DECL
#include "halo/lib/ir/fusion.cc.inc"
#undef HALO_FUSION_CMD_OPTIONS_DECL

static void PopulateCodeGenPasses(PassManager* pm, std::ostream* out_code,
                                  std::ostream* out_constants,
                                  std::ostream* out_header,
                                  bool is_c_or_cxx_output,
                                  bool is_binary_output) {
  auto constant_storage =
      GenericLLVMIRCodeGen::ConstantDataStorage::DefinedAsStatic;
  if (SeparateConstants) {
    constant_storage =
        GenericLLVMIRCodeGen::ConstantDataStorage::DeclaredAsExternal;
  }

  CodeGen* cg = nullptr;
  if (is_c_or_cxx_output) {
    Opts opts(EnableBF16 ? true : false);
    if (llvm::StringRef(Target).startswith_lower("cc")) {
      opts.dialect = Dialect::C99;
    }
    opts.print_mem_stats = PrintMemStats;
    opts.emit_value_reset = EmitValueReset;
    opts.exec_mode = ExecMode.getValue();
    opts.emit_value_id_as_int = EmitValueIDAsInt;
    opts.emit_inference_func_sig = EmitInferenceFunctionSignature;
    opts.emit_dynamic_batch = (Batch.getValue() == kDynamicBatchSize);
    cg = pm->AddPass<GenericCXXCodeGen>(std::ref(*out_code),
                                        std::ref(*out_header), opts);
    cg->SetAPI(Api);

    if (EmitDataAsC) {
      pm->AddPass<GenericCXXConstantWriter>(std::ref(*out_constants));
    } else {
      pm->AddPass<X86ConstantWriter>(std::ref(*out_constants));
    }
    if (EmitTritonConfig) {
      pm->AddPass<TritonConfigWriter>(TritonConfigFile.getValue());
    }
    return;
  }

  if (EmitLLVMIR) {
    cg = pm->AddPass<GenericLLVMIRCodeGen>(constant_storage);
    pm->AddPass<GenericLLVMIRWriter>(std::ref(*out_code), is_binary_output);
    if (SeparateConstants && !EmitCodeOnly) {
      pm->AddPass<GenericConstantWriter>(std::ref(*out_constants),
                                         is_binary_output);
    }
  } else {
    llvm::Triple triple(Target);
    switch (triple.getArch()) {
      case llvm::Triple::ArchType::x86:
      case llvm::Triple::ArchType::x86_64: {
        pm->AddPass<X86LLVMIRCodeGen>(
            GenericLLVMIRCodeGen::ConstantDataStorage::DeclaredAsExternal);
        pm->AddPass<X86BinaryWriter>(std::ref(*out_code));
        if (SeparateConstants && !EmitCodeOnly) {
          pm->AddPass<X86ConstantWriter>(std::ref(*out_constants));
        }
        break;
      }
      case llvm::Triple::ArchType::aarch64: {
        pm->AddPass<ARMLLVMIRCodeGen>(
            GenericLLVMIRCodeGen::ConstantDataStorage::DeclaredAsExternal);
        pm->AddPass<ARMBinaryWriter>(std::ref(*out_code));
        if (SeparateConstants && !EmitCodeOnly) {
          pm->AddPass<ARMConstantWriter>(std::ref(*out_constants));
        }
        break;
      }
      case llvm::Triple::ArchType::riscv32:
      case llvm::Triple::ArchType::riscv64: {
        if (RISCVOpt) {
          pm->AddPass<RISCVLLVMIRCodeGen>(
              GenericLLVMIRCodeGen::ConstantDataStorage::DeclaredAsExternal,
              "libRT_RISCV.a");
        } else {
          pm->AddPass<RISCVLLVMIRCodeGen>(
              GenericLLVMIRCodeGen::ConstantDataStorage::DeclaredAsExternal);
        }
        pm->AddPass<RISCVBinaryWriter>(std::ref(*out_code));
        if (SeparateConstants && !EmitCodeOnly) {
          pm->AddPass<RISCVConstantWriter>(std::ref(*out_constants));
        }

        break;
      }

      default: {
        HLCHECK(0 && "Unsupported");
      }
    }
  }
  if (cg != nullptr) {
    cg->SetAPI(Api);
  }
}

static void PopulatePasses(PassManager* pm, std::ostream* out_code,
                           std::ostream* out_constants,
                           std::ostream* out_header, bool is_c_or_cxx_output,
                           bool is_binary_output, Parser::Format format) {
  std::vector<std::string> input_shapes(InputsShape.begin(), InputsShape.end());
  pm->AddPass<InputLegalizer>(Batch.getValue(), input_shapes);
  if (!Outputs.empty()) {
    std::vector<std::string> outputs(Outputs.begin(), Outputs.end());
    pm->AddPass<OutputRewriter>(outputs);
  }
  if (format == Parser::Format::CAFFE) {
    pm->AddPass<CAFFEExtensionLegalizer>();
  } else if (format == Parser::Format::TENSORFLOW) {
    pm->AddPass<TFExtensionLegalizer>();
  } else {
    HLCHECK(format == Parser::Format::ONNX);
    pm->AddPass<ONNXExtensionLegalizer>();
  }
  pm->AddPass<DCE>();
  pm->AddPass<TypeLegalizer>(true);
  if (!Inputs.empty()) {
    std::vector<std::string> inputs(Inputs.begin(), Inputs.end());
    pm->AddPass<InputRewriter>(inputs);
  }

  pm->AddPass<InstSimplify>(
      llvm::StringRef(Target).startswith("cxx"), DisableBroadcasting.getValue(),
      RemoveInputTranspose.getValue(), RemoveOutputTranspose.getValue());
  if (ReorderChannelLayout != ReorderChannel::ChannelOrder::None) {
    pm->AddPass<ReorderChannel>(ReorderChannelLayout ==
                                ReorderChannel::ChannelOrder::ChannelFirst);
  }
  pm->AddPass<Fusion>(GetFusionOptions());
  if (SplitFunction) {
    pm->AddPass<Splitting>();
    pm->AddPass<DevicePlacement>();
  }

  PopulateCodeGenPasses(pm, out_code, out_constants, out_header,
                        is_c_or_cxx_output, is_binary_output);
}

static bool FormatCode(const std::string& filename) {
  if (filename.empty() || filename == "-") {
    return false;
  }
  // Search clang-format in PATH env.
  auto exe = llvm::sys::findProgramByName("clang-format", {});
  if (!exe) {
    exe = llvm::sys::findProgramByName("clang-format-9", {});
  }
  std::string ret_msg;
  if (exe) {
    ret_msg = "";
    const char* arg0 = "--style=LLVM";
    const char* arg1 = "-i"; // in-place format.
    constexpr int timeout = 10;
    llvm::sys::ExecuteAndWait(exe.get(), {arg0, arg1, filename}, {}, {},
                              timeout, 0, &ret_msg);
  } else {
    ret_msg = "Unable to find formatting tool";
  }
  if (!ret_msg.empty()) {
    std::cerr << "Code format failed: " << ret_msg << "\n";
  }
  return true;
}

static void PrintVersion(llvm::raw_ostream& os) {
  os << "  Version:\t" << HALO_MAJOR << '.' << HALO_MINOR << '.' << HALO_PATCH
     << '\n';
#ifndef NDEBUG
  os << "  Build:\tDebug\n";
#else
  os << "  Build:\tRelease\n";
#endif
}

/// Guess the model format based on input file extension.gg
static Parser::Format InferFormat(
    const llvm::cl::list<std::string>& model_files, size_t file_idx) {
  llvm::StringRef ext = llvm::sys::path::extension(model_files[file_idx]);
  auto format = llvm::StringSwitch<Parser::Format>(ext)
                    .Case(".pb", Parser::Format::TENSORFLOW)
                    .Case(".pbtxt", Parser::Format::TENSORFLOW)
                    .Case(".prototxt", Parser::Format::TENSORFLOW)
                    .Case(".onnx", Parser::Format::ONNX)
                    .Case(".json", Parser::Format::MXNET)
                    .Default(Parser::Format::INVALID);
  // Check the next input file to see if it is caffe.
  if (format == Parser::Format::TENSORFLOW &&
      (file_idx + 1 < model_files.size()) &&
      llvm::sys::path::extension(model_files[file_idx + 1]) == ".caffemodel") {
    format = Parser::Format::CAFFE;
  }
  return format;
}

static Status ParseModels(const llvm::cl::list<std::string>& model_files,
                          const llvm::cl::opt<Parser::Format>& model_format,
                          const llvm::cl::opt<std::string>& entry_func_name,
                          const armory::Opts& opts, Module* module,
                          Parser::Format* f) {
  std::set<std::string> func_names;
  for (size_t i = 0, e = model_files.size(); i < e; ++i) {
    Parser::Format format = model_format;
    if (format == Parser::Format::INVALID) {
      format = InferFormat(model_files, i);
    }
    HLCHECK(format != Parser::Format::INVALID);
    *f = format;
    FunctionBuilder func_builder(module);
    // Use stem of the input model as function name.
    std::string func_name = entry_func_name.empty()
                                ? llvm::sys::path::stem(model_files[i]).str()
                                : entry_func_name.getValue();
    while (func_names.count(func_name) != 0) {
      func_name.append("_").append(std::to_string(i));
    }
    func_names.insert(func_name);
    Function* func = func_builder.CreateFunction(func_name);
    std::vector<std::string> files{model_files[i]};
    if (format == Parser::Format::CAFFE || format == Parser::Format::MXNET) {
      HLCHECK(i + 1 < e);
      files.push_back(model_files[++i]);
    }
    if (Status status = Parser::Parse(func, format, files, opts);
        status != Status::SUCCESS) {
      return status;
    }
  }
  return Status::SUCCESS;
}

int main(int argc, char** argv) {
  llvm::cl::SetVersionPrinter(PrintVersion);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  GlobalContext ctx;
  ctx.SetBasePath(argv[0]);
  ctx.SetTargetTriple(Target);
  ctx.SetProcessorName(Processor);

  Module m(ctx, ModuleName);

  armory::Opts opts;
  Parser::Format format = Parser::Format::INVALID;
  if (ParseModels(ModelFiles, ModelFormat, EntryFunctionName, opts, &m,
                  &format) != Status::SUCCESS) {
    return 1;
  }

  if (PrintAll) {
    m.Dump();
  }

  PassManager pm(ctx);

  std::ofstream of_code;
  std::ofstream of_constants;
  std::ofstream of_header;
  std::ostream* out_code = &std::cout;
  std::ostream* out_constants = &std::cout;
  std::ostream* out_header = &std::cout;

  bool is_binary_output = false;
  llvm::StringRef target_name(Target);
  bool is_c_or_cxx_output =
      target_name.startswith_lower("cxx") || target_name.startswith_lower("cc");
  llvm::SmallString<128> header_file_name("");
  if (!OutputFile.empty() && OutputFile != "-") {
    of_code.open(OutputFile, std::ofstream::binary);
    out_code = &of_code;
    llvm::StringRef name(OutputFile);
    llvm::SmallString<128> data_file_name(name);
    header_file_name = name;
    is_binary_output = name.endswith(".bc") || name.endswith(".o");
    if (EmitDataAsC) {
      llvm::sys::path::replace_extension(data_file_name, "data.cc");
    } else {
      llvm::sys::path::replace_extension(data_file_name, ".bin");
    }
    llvm::sys::path::replace_extension(header_file_name, ".h");

    of_constants.open(data_file_name.str(), std::ofstream::binary);
    out_constants = &of_constants;
    of_header.open(header_file_name.str());
    out_header = &of_header;
  }

  if (EmitTritonConfig) {
    if (!TritonConfigFile.empty() &&
        llvm::sys::path::filename(TritonConfigFile).equals(TritonConfigFile)) {
      llvm::SmallString<128> file_name;
      llvm::sys::path::append(file_name,
                              llvm::sys::path::parent_path(OutputFile),
                              TritonConfigFile);
      TritonConfigFile = file_name.str();
    }
  }

  PopulatePasses(&pm, out_code, out_constants, out_header, is_c_or_cxx_output,
                 is_binary_output, format);
  if (is_c_or_cxx_output) {
    ctx.SetTargetTriple("x86_64"); // For binary constant writer.
  }

  auto status = pm.Run(&m);

  if (PrintAll) {
    m.Dump();
  }

  if (status != Status::SUCCESS) {
    return -1;
  }

  if (!DisableCodeFormat && is_c_or_cxx_output && of_code.good()) {
    of_code.close();
    FormatCode(OutputFile);
  }
  if (!DisableCodeFormat && of_header.good()) {
    of_header.close();
    FormatCode(header_file_name.str());
  }
  return 0;
}
