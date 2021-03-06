#include "token_collector_util.hpp"
#include "token_collector.hpp"
#include "xacc.hpp"
#include "xacc_service.hpp"
#include <limits>
#include <qalloc>

#include "qrt_mapper.hpp"

#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Token.h"
#include "clang/Sema/DeclSpec.h"

namespace qcor {

void set_verbose(bool verbose) { xacc::set_verbose(verbose); }
void info(const std::string &s) { xacc::info(s); }

std::pair<std::string, std::string>
run_token_collector(clang::Preprocessor &PP, clang::CachedTokens &Toks,
                    const std::string &function_prototype) {

  if (!xacc::isInitialized()) {
    xacc::Initialize();
  }

  auto all_token_collectors = xacc::getServices<TokenCollector>();
  auto all_compilers = xacc::getServices<xacc::Compiler>();
  std::string kernel_src = "", compiler_name = "";

  for (auto &tc : all_token_collectors) {
    xacc::info("Running the " + tc->name() + " token collector");
    std::stringstream tmp_ss;
    (*tc).collect(PP, Toks, tmp_ss);

    if (xacc::hasCompiler(tc->name())) {
      auto compiler = xacc::getCompiler(tc->name());

      kernel_src =
          "__qpu__ " + function_prototype + " {\n" + tmp_ss.str() + " }";
      if (compiler->canParse(kernel_src)) {
        xacc::info(compiler->name() + " could parse tokens generated by " +
                   tc->name());
        compiler_name = compiler->name();
        return std::make_pair(kernel_src, compiler_name);
      } else {
        xacc::info(compiler->name() + " could not parse the tokens.");
      }
    }
  }

  // if we make it here, its not good
  xacc::error("[qcor] Invalid QCOR kernel expression, could not parse with "
              "available SyntaxHandlers / XACC Compilers.");
  return std::make_pair(kernel_src, compiler_name);
}

void run_token_collector_llvm_rt(clang::Preprocessor &PP,
                                 clang::CachedTokens &Toks,
                                 const std::string &function_prototype,
                                 std::vector<std::string> bufferNames,
                                 const std::string &kernel_name,
                                 llvm::raw_string_ostream &OS,
                                 const std::string &qpu_name, int shots) {

  if (!xacc::isInitialized()) {
    xacc::Initialize();
  }

  // Used to have xasm return "", now " " needed
  // everywhere, being lazy here...
  auto add_spacing = [](const std::string language) {
    // if (language == "xasm") {
    //   return " ";
    // } else {
    return " ";
    // }
  };

  std::vector<std::pair<std::string, std::string>> classical_variables;

  // Programmers can specify the language by saying
  // using qcor::openqasm or something like that, default is xasm
  auto process_inst_stmt = [&](int &i, std::shared_ptr<xacc::Compiler> compiler,
                               clang::Token &current_token,
                               std::string &terminating_char,
                               std::string extra_preamble = "")
      -> std::pair<std::shared_ptr<xacc::Instruction>, std::string> {
    std::stringstream ss;
    auto current_token_str = PP.getSpelling(current_token);

    ss << extra_preamble;

    while (current_token_str != terminating_char) {
      ss << current_token_str << add_spacing(compiler->name());
      i++;
      current_token = Toks[i];
      current_token_str = PP.getSpelling(current_token);
    }

    if (ss.str().find("oracle") == std::string::npos) {
      ss << terminating_char;
    }

    // I want to store all classical variables, bc I need
    // to add them to the xacc kernel function prototype to
    // ensure canParse passes in certain cases
    if (ss.str().find("=") != std::string::npos) {
      // we have a classical var_type_tokens var_name = ...;
      int tmp_i = i;
      for (int j = tmp_i;; j--) {
        auto tok = Toks[j];
        if (tok.is(clang::tok::equal)) {
          auto var_name = PP.getSpelling(Toks[j - 1]);
          // get the var_type, which should be all
          // tokens back to last semi, l_brace, or r_brace, or if j == 0
          std::vector<std::string> tokens;
          for (int k = j - 2; k >= 0; k--) {
            if (Toks[k].is(clang::tok::semi) ||
                Toks[k].is(clang::tok::r_brace) ||
                Toks[k].is(clang::tok::l_brace)) {
              break;
            }
            if (PP.getSpelling(Toks[k]) != "const") {
              tokens.push_back(PP.getSpelling(Toks[k]));
            }
          }
          // reverse the tokens now and write them to
          // a string stream
          std::reverse(tokens.begin(), tokens.end());
          std::stringstream type_ss;
          for (auto &t : tokens) {
            type_ss << t;
          }
          auto var_type = type_ss.str();
          classical_variables.push_back({var_type, var_name});
          break;
        }
      }
    }

    auto tmp_func_proto = function_prototype;
    if (!classical_variables.empty()) {
      for (auto &[type, name] : classical_variables) {
        tmp_func_proto.insert(tmp_func_proto.length() - 1,
                              "," + type + " " + name);
      }
    }
    auto str_src = "__qpu__ " + tmp_func_proto + "{\n" + ss.str() + "\n}";

    // std::cout << "COMPILING\n"
    //           << tmp_func_proto + "{\n" + ss.str() + "\n}"
    //           << "\n";

    // If canParse, get the CompositeInst, if not, return the code
    // to be added to qrt_code
    if (compiler->canParse(str_src)) {
      return {compiler->compile(str_src)->getComposites()[0], ""};
    } else {
      return {nullptr, ss.str() + "\n"};
    }
  };

  std::function<void(int &, std::shared_ptr<xacc::Compiler>, clang::Token &,
                     clang::CachedTokens &, std::string &, std::stringstream &,
                     std::string)>
      process_for_block = [&](int &i, std::shared_ptr<xacc::Compiler> compiler,
                              clang::Token &current_token,
                              clang::CachedTokens &Toks,
                              std::string &terminating_char,
                              std::stringstream &qrt_code,
                              std::string extra_preamble) {
        // slurp up the for
        std::stringstream for_ss;

        // eat up the l_paren
        for_ss << "for (";
        int seen_l_paren = 1;
        i += 2;
        current_token = Toks[i];

        while (seen_l_paren > 0) {
          if (current_token.is(clang::tok::l_paren))
            seen_l_paren++;
          if (current_token.is(clang::tok::r_paren))
            seen_l_paren--;
          for_ss << PP.getSpelling(current_token) << " ";
          i++;
          current_token = Toks[i];
        }

        qrt_code << for_ss.str();
        // we could have for stmt with l_brace or without for a single inst
        if (current_token.is(clang::tok::l_brace)) {
          qrt_code << " {\n";

          // eat up the {
          i++;
          current_token = Toks[i];

          // Now loop through the for loop body
          int l_brace_count = 1;
          while (l_brace_count != 0) {
            // In here we have statements separated by compiler terminator
            // (default ';')
            // Note could have nested for stmts...
            if (current_token.is(clang::tok::kw_for)) {
              process_for_block(i, compiler, current_token, Toks,
                                terminating_char, qrt_code, extra_preamble);
            } else {
              auto [comp_inst, src_str] = process_inst_stmt(
                  i, compiler, current_token, terminating_char, extra_preamble);
              if (comp_inst) {
                auto visitor = std::make_shared<qrt_mapper>(comp_inst->name());
                xacc::InstructionIterator iter(comp_inst);
                while (iter.hasNext()) {
                  auto next = iter.next();
                  next->accept(visitor);
                }
                qrt_code << visitor->get_new_src();
              } else {
                qrt_code << src_str;
              }
            }
            // missing ';', eat it up too
            i++;
            current_token = Toks[i];

            if (current_token.is(clang::tok::l_brace)) {
              l_brace_count++;
            }

            if (current_token.is(clang::tok::r_brace)) {
              l_brace_count--;
            }
          }

          qrt_code << "}\n";

        } else {
          // Here we don't have a l_brace, so we just have the one
          // quantum instruction

          qrt_code << "\n   ";

          auto [comp_inst, src_str] = process_inst_stmt(
              i, compiler, current_token, terminating_char, extra_preamble);
          if (comp_inst) {
            auto visitor = std::make_shared<qrt_mapper>(comp_inst->name());
            xacc::InstructionIterator iter(comp_inst);
            while (iter.hasNext()) {
              auto next = iter.next();
              next->accept(visitor);
            }
            qrt_code << visitor->get_new_src();
          } else {
            qrt_code << src_str;
          }
        }
      };

  auto compiler = xacc::getCompiler("xasm");
  auto terminating_char = compiler->get_statement_terminator();

  std::stringstream qrt_code;
  std::string extra_preamble = "", language = "xasm";
  std::map<std::string, std::string> oracle_name_to_extra_preamble;
  std::map<std::string, int> creg_name_to_size;
  int countQregs = 0;

  for (int i = 0; i < Toks.size(); i++) {
    auto current_token = Toks[i];
    auto current_token_str = PP.getSpelling(current_token);

    if (current_token.is(clang::tok::kw_using)) {
      // Found using
      // i+3 bc we skip using, qcor and ::;
      language = PP.getSpelling(Toks[i + 3]);
      if (language == "openqasm") {
        // use staq
        language = xacc::hasCompiler("staq") ? "staq" : "openqasm";

        std::stringstream sss;
        for (auto &b : bufferNames) {
          // sss << "qreg " << b << "[100];\n";
          // Note - we don't know the size of the buffer
          // at this point, so just create one with max size
          // and we can provide an IR Pass later that updates it
          auto q = qalloc(std::numeric_limits<int>::max());
          q.setNameAndStore(b.c_str());
        }
        extra_preamble += sss.str();
      }

      compiler = xacc::getCompiler(language);
      terminating_char = compiler->get_statement_terminator();
      // +4 to skip ';' too
      i = i + 4;
      continue;
    }

    if (current_token_str == "oracle") {
      if (language != "staq") {
        xacc::error("Error - must specify 'using qcor::openqasm;' before using "
                    "staq openqasm code.");
      }
      std::stringstream slurp_oracle;
      i++;
      current_token = Toks[i];
      std::string oracle_name = PP.getSpelling(current_token);
      slurp_oracle << "oracle " << oracle_name << " ";

      while (true) {

        i++;
        current_token = Toks[i];
        slurp_oracle << PP.getSpelling(current_token);

        if (current_token.is(clang::tok::r_brace)) {
          break;
        }
      }

      auto preamble = slurp_oracle.str() + "\n";

      oracle_name_to_extra_preamble.insert({oracle_name, preamble});
      continue;
    }

    if (oracle_name_to_extra_preamble.count(current_token_str)) {
      // add the appropriate preamble here
      extra_preamble += oracle_name_to_extra_preamble[current_token_str];
      auto [comp_inst, src_str] = process_inst_stmt(
          i, compiler, current_token, terminating_char, extra_preamble);
      if (comp_inst) {
        auto visitor = std::make_shared<qrt_mapper>(comp_inst->name());
        xacc::InstructionIterator iter(comp_inst);
        while (iter.hasNext()) {
          auto next = iter.next();
          next->accept(visitor);
        }
        qrt_code << visitor->get_new_src();
      } else {
        qrt_code << src_str;
      }
      continue;
    }

    if (current_token_str == "qreg") {

      // allocate called within kernel, likely with openqasm
      // get the size and allocated it, but dont add to kernel string

      // skip qreg
      i++;
      current_token = Toks[i];

      // get qreg var name
      auto variable_name = PP.getSpelling(current_token);

      // skip [
      i += 2;
      current_token = Toks[i];

      auto size = std::stoi(PP.getSpelling(current_token));

      // skip ] and ;
      i += 2;

      auto q = qalloc(size);
      q.setNameAndStore(variable_name.c_str());

      qrt_code << "auto " << variable_name << " = " << bufferNames[countQregs]
               << ";\n";

      classical_variables.push_back({"qreg", variable_name});
      countQregs++;

      continue;
    }

    if (current_token_str == "OPENQASM") {
      i += 2;
      continue;
    }

    if (current_token_str == "measure") {
      // we have an ibm style measure,
      // so make sure that we map to individual measures
      // since we don't know the size of the qreg

      // next token is qreg name
      i++;
      current_token = Toks[i];
      current_token_str = PP.getSpelling(current_token);
      auto qreg_name = current_token_str;

      // next token could be [ or could be ->
      i++;
      current_token = Toks[i];
      if (current_token.is(clang::tok::l_square)) {
        i--;
        i--;
        current_token = Toks[i];
        // This we can parse, so just eat it up and get the Measure IR node out
        auto [comp_inst, src_str] = process_inst_stmt(
            i, compiler, current_token, terminating_char, extra_preamble);
        if (comp_inst) {
          auto visitor = std::make_shared<qrt_mapper>(comp_inst->name());
          xacc::InstructionIterator iter(comp_inst);
          while (iter.hasNext()) {
            auto next = iter.next();
            next->accept(visitor);
          }
          qrt_code << visitor->get_new_src();
        } else {
          qrt_code << src_str;
        }
        continue;
      } else {
        // the token is ->

        // the next one is the creg name
        i++;
        current_token = Toks[i];
        current_token_str = PP.getSpelling(current_token);
        auto creg_name = current_token_str;
        auto size = creg_name_to_size[creg_name];
        for (int k = 0; k < size; k++) {
          qrt_code << "quantum::mz(" << qreg_name << "[" << k << "]);\n";
        }
        continue;
      }
    }

    if (current_token_str == "creg") {

      auto creg_name = PP.getSpelling(Toks[i + 1]);
      auto creg_size = PP.getSpelling(Toks[i + 3]);

      creg_name_to_size.insert({creg_name, std::stoi(creg_size)});

      std::stringstream sss;
      while (current_token.isNot(clang::tok::semi)) {
        sss << current_token_str << " ";
        i++;
        current_token = Toks[i];
        current_token_str = PP.getSpelling(current_token);
      }

      extra_preamble += sss.str() + ";\n";

      continue;
    }

    // If we find a for stmt...
    if (current_token.is(clang::tok::kw_for)) {

      process_for_block(i, compiler, current_token, Toks, terminating_char,
                        qrt_code, extra_preamble);
      //   std::cout << "out of for loop now, current is "
      //             << PP.getSpelling(current_token) << "\n";
      continue;
    }

    if (current_token.is(clang::tok::kw_if)) {
    }

    // this is a quantum statement + terminating char
    // slurp up to the terminating char
    auto [comp_inst, src_str] = process_inst_stmt(
        i, compiler, current_token, terminating_char, extra_preamble);
    if (comp_inst) {
      auto visitor = std::make_shared<qrt_mapper>(comp_inst->name());
      xacc::InstructionIterator iter(comp_inst);
      while (iter.hasNext()) {
        auto next = iter.next();
        next->accept(visitor);
      }
      qrt_code << visitor->get_new_src();
    } else {
      qrt_code << src_str;
    }
  }

  //   std::cout << "QRT CODE:\n" << qrt_code.str() << "\n";

  OS << "quantum::initialize(\"" << qpu_name << "\", \"" << kernel_name
     << "\");\n";
  for (auto &buf : bufferNames) {
    OS << buf << ".setNameAndStore(\"" + buf + "\");\n";
  }

  if (!oracle_name_to_extra_preamble.empty()) {
    // we had an oracle synthesis, just add an
    // anc registry preemptively
    OS << "auto anc = qalloc(" << std::numeric_limits<int>::max() << ");\n";
  }
  if (shots > 0) {
    OS << "quantum::set_shots(" << shots << ");\n";
  }
  OS << qrt_code.str();
  OS << "if (__execute) {\n";

  if (bufferNames.size() > 1) {
    OS << "xacc::AcceleratorBuffer * buffers[" << bufferNames.size() << "] = {";
    OS << bufferNames[0] << ".results()";
    for (unsigned int k = 1; k < bufferNames.size(); k++) {
      OS << ", " << bufferNames[k] << ".results()";
    }
    OS << "};\n";
    OS << "quantum::submit(buffers," << bufferNames.size();
  } else {
    OS << "quantum::submit(" << bufferNames[0] << ".results()";
  }

  OS << ");\n";
  OS << "}";

  // In runtime mode, we contribute each annotated *kernel* as a circuit.
  // Hence, kernels can be used within other kernels similar to the way
  // XACC circuits are instantiated in XASM.
  auto circuit = std::shared_ptr<xacc::Instruction>(
      new xacc::quantum::Circuit(kernel_name));
  xacc::contributeService(kernel_name, circuit);
}

} // namespace qcor

// FIXME may want this later
// if (current_token_str == "qreg") {

//   // allocate called within kernel, likely with openqasm
//   // get the size and allocated it, but dont add to kernel string

//   // skip qreg
//   i++;
//   current_token = Toks[i];

//   // get qreg var name
//   auto variable_name = PP.getSpelling(current_token);

//   // skip [
//   i += 2;
//   current_token = Toks[i];

//   std::cout << variable_name
//             << ", CURRENT: " << PP.getSpelling(current_token) << "\n";
//   auto size = std::stoi(PP.getSpelling(current_token));

//   // skip ] and ;
//   i += 2;
//   std::cout << "NOW WE ARE " << PP.getSpelling(Toks[i]) << "\n";

//   auto q = qalloc(size);
//   q.setNameAndStore(variable_name.c_str());

//   // Update function_prototype FIXME
//   continue;
// }

// slurp up the for
//   std::stringstream for_ss;

//   // eat up the l_paren
//   for_ss << "for (";
//   int seen_l_paren = 1;
//   i += 2;
//   current_token = Toks[i];

//   while (seen_l_paren > 0) {
//     if (current_token.is(clang::tok::l_paren))
//       seen_l_paren++;
//     if (current_token.is(clang::tok::r_paren))
//       seen_l_paren--;
//     for_ss << PP.getSpelling(current_token) << " ";
//     i++;
//     current_token = Toks[i];
//   }

//   qrt_code << for_ss.str();

//   // we could have for stmt with l_brace or without for a single inst
//   if (current_token.is(clang::tok::l_brace)) {
//     qrt_code << " {\n";

//     // eat up the {
//     i++;
//     current_token = Toks[i];

//     // Now loop through the for loop body
//     int l_brace_count = 1;
//     while (l_brace_count != 0) {
//       // In here we have statements separated by compiler terminator
//       // (default ';')
//       auto [comp_inst, src_str] = process_inst_stmt(
//           i, compiler, current_token, terminating_char,
//           extra_preamble);
//       {
//         auto visitor = std::make_shared<qrt_mapper>();
//         xacc::InstructionIterator iter(comp_inst);
//         while (iter.hasNext()) {
//           auto next = iter.next();
//           if (!next->isComposite()) {
//             next->accept(visitor);
//           }
//         }
//         // inst->accept(visitor);
//         qrt_code << visitor->get_new_src();
//       }
//       // missing ';', eat it up too
//       i++;
//       current_token = Toks[i];

//       if (current_token.is(clang::tok::l_brace)) {
//         l_brace_count++;
//       }

//       if (current_token.is(clang::tok::r_brace)) {
//         l_brace_count--;
//       }
//     }

//     // now eat the r_brace
//     i++;
//     // current_token = Toks[i];
//     qrt_code << "}\n";

//     continue;
//   } else {
//     // Here we don't have a l_brace, so we just have the one
//     // quantum instruction

//     qrt_code << "\n   ";

//     auto [comp_inst, src_str] = process_inst_stmt(
//         i, compiler, current_token, terminating_char, extra_preamble);
//     if (comp_inst) {
//       auto visitor = std::make_shared<qrt_mapper>();
//       xacc::InstructionIterator iter(comp_inst);
//       while (iter.hasNext()) {
//         auto next = iter.next();
//         if (!next->isComposite()) {
//           next->accept(visitor);
//         }
//       }
//       qrt_code << visitor->get_new_src();
//     } else {
//       qrt_code << src_str;
//     }

//     // missing ';', eat it up too
//     i++;
//     current_token = Toks[i];
//   }