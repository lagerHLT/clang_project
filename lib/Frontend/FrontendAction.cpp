//===--- FrontendAction.cpp -----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/FrontendAction.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclGroup.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/LayoutOverrideSource.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Serialization/ASTDeserializationListener.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/GlobalModuleIndex.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <system_error>

// TASKIFY
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <functional>
#include <cctype>
#include <locale>
#include <fstream>

using namespace clang;

template class llvm::Registry<clang::PluginASTAction>;

namespace {

class DelegatingDeserializationListener : public ASTDeserializationListener {
  ASTDeserializationListener *Previous;
  bool DeletePrevious;

public:
  explicit DelegatingDeserializationListener(
      ASTDeserializationListener *Previous, bool DeletePrevious)
      : Previous(Previous), DeletePrevious(DeletePrevious) {}
  ~DelegatingDeserializationListener() override {
    if (DeletePrevious)
      delete Previous;
  }

  void ReaderInitialized(ASTReader *Reader) override {
    if (Previous)
      Previous->ReaderInitialized(Reader);
  }
  void IdentifierRead(serialization::IdentID ID,
                      IdentifierInfo *II) override {
    if (Previous)
      Previous->IdentifierRead(ID, II);
  }
  void TypeRead(serialization::TypeIdx Idx, QualType T) override {
    if (Previous)
      Previous->TypeRead(Idx, T);
  }
  void DeclRead(serialization::DeclID ID, const Decl *D) override {
    if (Previous)
      Previous->DeclRead(ID, D);
  }
  void SelectorRead(serialization::SelectorID ID, Selector Sel) override {
    if (Previous)
      Previous->SelectorRead(ID, Sel);
  }
  void MacroDefinitionRead(serialization::PreprocessedEntityID PPID,
                           MacroDefinition *MD) override {
    if (Previous)
      Previous->MacroDefinitionRead(PPID, MD);
  }
};

/// \brief Dumps deserialized declarations.
class DeserializedDeclsDumper : public DelegatingDeserializationListener {
public:
  explicit DeserializedDeclsDumper(ASTDeserializationListener *Previous,
                                   bool DeletePrevious)
      : DelegatingDeserializationListener(Previous, DeletePrevious) {}

  void DeclRead(serialization::DeclID ID, const Decl *D) override {
    llvm::outs() << "PCH DECL: " << D->getDeclKindName();
    if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
      llvm::outs() << " - " << *ND;
    llvm::outs() << "\n";

    DelegatingDeserializationListener::DeclRead(ID, D);
  }
};

/// \brief Checks deserialized declarations and emits error if a name
/// matches one given in command-line using -error-on-deserialized-decl.
class DeserializedDeclsChecker : public DelegatingDeserializationListener {
  ASTContext &Ctx;
  std::set<std::string> NamesToCheck;

public:
  DeserializedDeclsChecker(ASTContext &Ctx,
                           const std::set<std::string> &NamesToCheck,
                           ASTDeserializationListener *Previous,
                           bool DeletePrevious)
      : DelegatingDeserializationListener(Previous, DeletePrevious), Ctx(Ctx),
        NamesToCheck(NamesToCheck) {}

  void DeclRead(serialization::DeclID ID, const Decl *D) override {
    if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
      if (NamesToCheck.find(ND->getNameAsString()) != NamesToCheck.end()) {
        unsigned DiagID
          = Ctx.getDiagnostics().getCustomDiagID(DiagnosticsEngine::Error,
                                                 "%0 was deserialized");
        Ctx.getDiagnostics().Report(Ctx.getFullLoc(D->getLocation()), DiagID)
            << ND->getNameAsString();
      }

    DelegatingDeserializationListener::DeclRead(ID, D);
  }
};

} // end anonymous namespace

FrontendAction::FrontendAction() : Instance(nullptr) {}

FrontendAction::~FrontendAction() {}

void FrontendAction::setCurrentInput(const FrontendInputFile &CurrentInput,
                                     std::unique_ptr<ASTUnit> AST) {
  this->CurrentInput = CurrentInput;
  CurrentASTUnit = std::move(AST);
}

std::unique_ptr<ASTConsumer>
FrontendAction::CreateWrappedASTConsumer(CompilerInstance &CI,
                                         StringRef InFile) {
  std::unique_ptr<ASTConsumer> Consumer = CreateASTConsumer(CI, InFile);
  if (!Consumer)
    return nullptr;

  if (CI.getFrontendOpts().AddPluginActions.size() == 0)
    return Consumer;

  // Make sure the non-plugin consumer is first, so that plugins can't
  // modifiy the AST.
  std::vector<std::unique_ptr<ASTConsumer>> Consumers;
  Consumers.push_back(std::move(Consumer));

  for (size_t i = 0, e = CI.getFrontendOpts().AddPluginActions.size();
       i != e; ++i) { 
    // This is O(|plugins| * |add_plugins|), but since both numbers are
    // way below 50 in practice, that's ok.
    for (FrontendPluginRegistry::iterator
        it = FrontendPluginRegistry::begin(),
        ie = FrontendPluginRegistry::end();
        it != ie; ++it) {
      if (it->getName() != CI.getFrontendOpts().AddPluginActions[i])
        continue;
      std::unique_ptr<PluginASTAction> P = it->instantiate();
      if (P->ParseArgs(CI, CI.getFrontendOpts().AddPluginArgs[i]))
        Consumers.push_back(P->CreateASTConsumer(CI, InFile));
    }
  }

  return llvm::make_unique<MultiplexConsumer>(std::move(Consumers));
}

bool FrontendAction::BeginSourceFile(CompilerInstance &CI,
                                     const FrontendInputFile &Input) {
  assert(!Instance && "Already processing a source file!");
  assert(!Input.isEmpty() && "Unexpected empty filename!");
  setCurrentInput(Input);
  setCompilerInstance(&CI);

  StringRef InputFile = Input.getFile();
  bool HasBegunSourceFile = false;
  if (!BeginInvocation(CI))
    goto failure;

  // AST files follow a very different path, since they share objects via the
  // AST unit.
  if (Input.getKind() == IK_AST) {
    assert(!usesPreprocessorOnly() &&
           "Attempt to pass AST file to preprocessor only action!");
    assert(hasASTFileSupport() &&
           "This action does not have AST file support!");

    IntrusiveRefCntPtr<DiagnosticsEngine> Diags(&CI.getDiagnostics());

    std::unique_ptr<ASTUnit> AST =
        ASTUnit::LoadFromASTFile(InputFile, Diags, CI.getFileSystemOpts());

    if (!AST)
      goto failure;

    // Inform the diagnostic client we are processing a source file.
    CI.getDiagnosticClient().BeginSourceFile(CI.getLangOpts(), nullptr);
    HasBegunSourceFile = true;

    // Set the shared objects, these are reset when we finish processing the
    // file, otherwise the CompilerInstance will happily destroy them.
    CI.setFileManager(&AST->getFileManager());
    CI.setSourceManager(&AST->getSourceManager());
    CI.setPreprocessor(&AST->getPreprocessor());
    CI.setASTContext(&AST->getASTContext());

    setCurrentInput(Input, std::move(AST));

    // Initialize the action.
    if (!BeginSourceFileAction(CI, InputFile))
      goto failure;

    // Create the AST consumer.
    CI.setASTConsumer(CreateWrappedASTConsumer(CI, InputFile));
    if (!CI.hasASTConsumer())
      goto failure;

    return true;
  }

  if (!CI.hasVirtualFileSystem()) {
    if (IntrusiveRefCntPtr<vfs::FileSystem> VFS =
          createVFSFromCompilerInvocation(CI.getInvocation(),
                                          CI.getDiagnostics()))
      CI.setVirtualFileSystem(VFS);
    else
      goto failure;
  }

  // Set up the file and source managers, if needed.
  if (!CI.hasFileManager())
    CI.createFileManager();
  if (!CI.hasSourceManager())
    CI.createSourceManager(CI.getFileManager());

  // IR files bypass the rest of initialization.
  if (Input.getKind() == IK_LLVM_IR) {
    assert(hasIRSupport() &&
           "This action does not have IR file support!");

    // Inform the diagnostic client we are processing a source file.
    CI.getDiagnosticClient().BeginSourceFile(CI.getLangOpts(), nullptr);
    HasBegunSourceFile = true;

    // Initialize the action.
    if (!BeginSourceFileAction(CI, InputFile))
      goto failure;

    // Initialize the main file entry.
    if (!CI.InitializeSourceManager(CurrentInput))
      goto failure;

    return true;
  }

  // If the implicit PCH include is actually a directory, rather than
  // a single file, search for a suitable PCH file in that directory.
  if (!CI.getPreprocessorOpts().ImplicitPCHInclude.empty()) {
    FileManager &FileMgr = CI.getFileManager();
    PreprocessorOptions &PPOpts = CI.getPreprocessorOpts();
    StringRef PCHInclude = PPOpts.ImplicitPCHInclude;
    std::string SpecificModuleCachePath = CI.getSpecificModuleCachePath();
    if (const DirectoryEntry *PCHDir = FileMgr.getDirectory(PCHInclude)) {
      std::error_code EC;
      SmallString<128> DirNative;
      llvm::sys::path::native(PCHDir->getName(), DirNative);
      bool Found = false;
      for (llvm::sys::fs::directory_iterator Dir(DirNative, EC), DirEnd;
           Dir != DirEnd && !EC; Dir.increment(EC)) {
        // Check whether this is an acceptable AST file.
        if (ASTReader::isAcceptableASTFile(Dir->path(), FileMgr,
                                           CI.getLangOpts(),
                                           CI.getTargetOpts(),
                                           CI.getPreprocessorOpts(),
                                           SpecificModuleCachePath)) {
          PPOpts.ImplicitPCHInclude = Dir->path();
          Found = true;
          break;
        }
      }

      if (!Found) {
        CI.getDiagnostics().Report(diag::err_fe_no_pch_in_dir) << PCHInclude;
        return true;
      }
    }
  }

  // Set up the preprocessor if needed. When parsing model files the
  // preprocessor of the original source is reused.
  if (!isModelParsingAction())
    CI.createPreprocessor(getTranslationUnitKind());

  // Inform the diagnostic client we are processing a source file.
  CI.getDiagnosticClient().BeginSourceFile(CI.getLangOpts(),
                                           &CI.getPreprocessor());
  HasBegunSourceFile = true;

  // Initialize the action.
  if (!BeginSourceFileAction(CI, InputFile))
    goto failure;

  // Initialize the main file entry. It is important that this occurs after
  // BeginSourceFileAction, which may change CurrentInput during module builds.
  if (!CI.InitializeSourceManager(CurrentInput))
    goto failure;

  // Create the AST context and consumer unless this is a preprocessor only
  // action.
  if (!usesPreprocessorOnly()) {
    // Parsing a model file should reuse the existing ASTContext.
    if (!isModelParsingAction())
      CI.createASTContext();

    std::unique_ptr<ASTConsumer> Consumer =
        CreateWrappedASTConsumer(CI, InputFile);
    if (!Consumer)
      goto failure;

    // FIXME: should not overwrite ASTMutationListener when parsing model files?
    if (!isModelParsingAction())
      CI.getASTContext().setASTMutationListener(Consumer->GetASTMutationListener());

    if (!CI.getPreprocessorOpts().ChainedIncludes.empty()) {
      // Convert headers to PCH and chain them.
      IntrusiveRefCntPtr<ExternalSemaSource> source, FinalReader;
      source = createChainedIncludesSource(CI, FinalReader);
      if (!source)
        goto failure;
      CI.setModuleManager(static_cast<ASTReader *>(FinalReader.get()));
      CI.getASTContext().setExternalSource(source);
    } else if (!CI.getPreprocessorOpts().ImplicitPCHInclude.empty()) {
      // Use PCH.
      assert(hasPCHSupport() && "This action does not have PCH support!");
      ASTDeserializationListener *DeserialListener =
          Consumer->GetASTDeserializationListener();
      bool DeleteDeserialListener = false;
      if (CI.getPreprocessorOpts().DumpDeserializedPCHDecls) {
        DeserialListener = new DeserializedDeclsDumper(DeserialListener,
                                                       DeleteDeserialListener);
        DeleteDeserialListener = true;
      }
      if (!CI.getPreprocessorOpts().DeserializedPCHDeclsToErrorOn.empty()) {
        DeserialListener = new DeserializedDeclsChecker(
            CI.getASTContext(),
            CI.getPreprocessorOpts().DeserializedPCHDeclsToErrorOn,
            DeserialListener, DeleteDeserialListener);
        DeleteDeserialListener = true;
      }
      CI.createPCHExternalASTSource(
          CI.getPreprocessorOpts().ImplicitPCHInclude,
          CI.getPreprocessorOpts().DisablePCHValidation,
          CI.getPreprocessorOpts().AllowPCHWithCompilerErrors, DeserialListener,
          DeleteDeserialListener);
      if (!CI.getASTContext().getExternalSource())
        goto failure;
    }

    CI.setASTConsumer(std::move(Consumer));
    if (!CI.hasASTConsumer())
      goto failure;
  }

  // Initialize built-in info as long as we aren't using an external AST
  // source.
  if (!CI.hasASTContext() || !CI.getASTContext().getExternalSource()) {
    Preprocessor &PP = CI.getPreprocessor();

    // If modules are enabled, create the module manager before creating
    // any builtins, so that all declarations know that they might be
    // extended by an external source.
    if (CI.getLangOpts().Modules)
      CI.createModuleManager();

    PP.getBuiltinInfo().InitializeBuiltins(PP.getIdentifierTable(),
                                           PP.getLangOpts());
  } else {
    // FIXME: If this is a problem, recover from it by creating a multiplex
    // source.
    assert((!CI.getLangOpts().Modules || CI.getModuleManager()) &&
           "modules enabled but created an external source that "
           "doesn't support modules");
  }

  // If we were asked to load any module map files, do so now.
  for (const auto &Filename : CI.getFrontendOpts().ModuleMapFiles) {
    if (auto *File = CI.getFileManager().getFile(Filename))
      CI.getPreprocessor().getHeaderSearchInfo().loadModuleMapFile(
          File, /*IsSystem*/false);
    else
      CI.getDiagnostics().Report(diag::err_module_map_not_found) << Filename;
  }

  // If we were asked to load any module files, do so now.
  for (const auto &ModuleFile : CI.getFrontendOpts().ModuleFiles)
    if (!CI.loadModuleFile(ModuleFile))
      goto failure;

  // If there is a layout overrides file, attach an external AST source that
  // provides the layouts from that file.
  if (!CI.getFrontendOpts().OverrideRecordLayoutsFile.empty() && 
      CI.hasASTContext() && !CI.getASTContext().getExternalSource()) {
    IntrusiveRefCntPtr<ExternalASTSource> 
      Override(new LayoutOverrideSource(
                     CI.getFrontendOpts().OverrideRecordLayoutsFile));
    CI.getASTContext().setExternalSource(Override);
  }

  return true;

  // If we failed, reset state since the client will not end up calling the
  // matching EndSourceFile().
  failure:
  if (isCurrentFileAST()) {
    CI.setASTContext(nullptr);
    CI.setPreprocessor(nullptr);
    CI.setSourceManager(nullptr);
    CI.setFileManager(nullptr);
  }

  if (HasBegunSourceFile)
    CI.getDiagnosticClient().EndSourceFile();
  CI.clearOutputFiles(/*EraseFiles=*/true);
  setCurrentInput(FrontendInputFile());
  setCompilerInstance(nullptr);
  return false;
}

//TASKIFY
// it capitalizes the first letter of a string
void Capitalize(std::string &s)
{
	bool cap = true;

	for (unsigned int i = 0; i <= s.length(); i++)
	{
		if (isalpha(s[i]) && cap == true)
		{
			s[i] = toupper(s[i]);
			cap = false;
		}
		else if (isspace(s[i]))
		{
			cap = true;
		}
	}
}

bool isCompleteFunctionName(std::string funcBody, int charStart, int nameLength){
	//is the found functionname not the start of the identifier
	if (charStart > 0 &&
		(isalpha(funcBody[charStart - 1]) ||
		isdigit(funcBody[charStart - 1]) ||
		funcBody[charStart - 1] == '_'))
	{
		return false;
	}

	funcBody = funcBody.substr(charStart);

	//is the found functionname not the end of the identifier 
	if (funcBody[nameLength] != '('){
		return false;
	}

	return true;
}

#include <sstream>
std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim)) {
		elems.push_back(item);
	}
	return elems;
}

std::vector<std::string> split(const std::string &s, char delim) {
	std::vector<std::string> elems;
	split(s, delim, elems);
	return elems;
}

bool replace(std::string& str, const std::string& from, const std::string& to)
{
	size_t start_pos = str.find(from);
	if (start_pos == std::string::npos)
		return false;
	str.replace(start_pos, from.length(), to);
	return true;
}

void CreateOutputFile(std::string outName, std::string fileName, std::string parameters)
{
	std::string className = outName;
	std::ofstream outFile;
	outFile.open(fileName);//, std::ios_base::app);
	

	//(xdata&A, xdata&b, xdata&ab) == (A,b,ab)
	std::string clean_parameters = parameters;
	while (replace(clean_parameters, "XData", "") == true);
	while (replace(clean_parameters, "&", "") == true);
	replace(clean_parameters, ")", "");
	int param_count = split(clean_parameters, ',').size();

	//beginning of file
	Capitalize(className);
	outFile << "class Generic" << className << "{\n";
	outFile << "public:\n";
	outFile << "\tstatic void base(";

	// body of finest function here
	outFile << parameters + "\n{";
	outFile << "\t FINEST_LEVEL_BODY;";
	outFile << "\n}\n";

	// other part
	outFile << "static void kernel(XTask *t){ \n";
	outFile << " if ( FinestLevel(t) )\n";
	outFile << "   base(t);\n";
	outFile << "  else\n";
	outFile << "   algorithm(t);\n";
	outFile << "};\n";

	// remove the last )
	replace(parameters.begin(), parameters.end(), ')', ' ');

	outFile << "static void algorithm(" + parameters + ", XTask *t)\n{";
	outFile << /*functionBody*/"ALGORITHM_BODY;";
	outFile << "\n";

	// add the last )
	parameters += ")";

	// other part
	outFile << "\talgorithm_end(t);\n";
	outFile << "}\n";
	outFile << "static void algorithm(XTask *t){\n";
	if (param_count == 1){
		outFile << "  unpack(" + clean_parameters + ");\n";
	}
	else{
		outFile << "  unpack";
		outFile << std::to_string(param_count);
		outFile << "(" + clean_parameters + ");\n";
	}

	outFile << "  algorithm(" + clean_parameters + ", t); \n";
	outFile << "}\n";
	outFile << "static void base(XTask *t){\n";

	if (param_count == 1){
		outFile << "  unpack(" + clean_parameters + ");\n";
	}
	else{
		outFile << "  unpack";
		outFile << std::to_string(param_count);
		outFile << "(" + clean_parameters + ");\n";
	}

	outFile << "  base(" + clean_parameters + "); \n";
	outFile << "}\n";
	outFile << "void operator()(" + parameters + "{\n";

	outFile << "  run(" + clean_parameters + "); \n";
	outFile << "}\n";
	outFile << "static void run(" + parameters + "{\n";

	outFile << "  submit_task(" + clean_parameters + "); \n";
	outFile << "}\n";
	outFile << "};\n";

	Capitalize(className);
	outFile << "Generic" + className + " " + outName + ";\n";

	// close file
	outFile.close();
}

void CreateTranslatedFile(std::string fileName, std::vector<std::string> includes, std::string main_function){
	std::ofstream outFile;
	outFile.open(fileName, std::ios_base::app);

	//Add includes
	for (int i = 0; i < includes.size(); i++){
		outFile << "#include ";
		outFile << "\"" + includes[i] + "\"";
	}

	//new line
	outFile << "\n";

	//add main
	outFile << main_function;

	// close file
	outFile.close();
}

// trim from start
static inline std::string &ltrim(std::string &s) {
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
	return s;
}

std::string retrieveFunctionBody(std::string result, std::string functionName)
{
	std::string functionBody = "";
	int char_position = 0;
	while (char_position >= 0)
	{
		char_position = result.find(functionName);
		if (char_position != -1)//if functionname is found
		{
			//is the found functionname not the start of the identifier. break
			if (!isCompleteFunctionName(result, char_position, functionName.length()))
			{
				result = result.substr(char_position + 1); //move 1 step to ignore this name in the future
				continue;
			}

			//we found a function. Drop the tail of the string
			result = result.substr(char_position);

			if (char_position = result.find(")"))//find next ")"
			{
				result = result.substr(char_position + 1);

				//remove any white spaces
				result = ltrim(result);

				if (result[0] == '{')//it's a function
				{
					//remove "{"
					result = result.substr(1);

					//save everything until statement is closed
					std::string tokName;
					int bracketsCounter = 0;
					int i = 0;
					while (!(result[i] == '}' && bracketsCounter == 0))
					{
						tokName = result[i];
						if (tokName == "{")
							bracketsCounter++;
						else if (tokName == "}")
							bracketsCounter--;
						functionBody += tokName;
						i++;
					}
					break;
				}
			}
		}
	}
	return functionBody;
}

void removeEntireFunction(std::string &fileCode, std::string functionName)
{
	int char_position = 0;
	char_position = fileCode.find(" " + functionName + "(");

	while (char_position != -1)
	{
		//char_position = fileCode.find(functionName);
		int funcName_start = char_position;
		//is the found functionname not the start of the identifier. break
		if (!isCompleteFunctionName(fileCode, char_position, functionName.length()))
		{
			//entire_function = fileCode.substr(char_position + 1); //move 1 step to ignore this name in the future
			char_position++;
			continue;
		}

		if (char_position = fileCode.find(")", funcName_start))//find next ")"
		{
			//find first {
			while (!(fileCode[char_position] == '{') ){
				char_position++;
			}

			if (fileCode[char_position] == '{')//it's a function
			{
				//save everything until statement is closed
				std::string tokName;
				char_position++;
				int bracketsCounter = 0;
				while (!(fileCode[char_position] == '}' && bracketsCounter == 0))
				{
					tokName = fileCode[char_position];
					if (tokName == "{")
						bracketsCounter++;
					else if (tokName == "}")
						bracketsCounter--;
					char_position++;
				}

				//find the start of the return type
				int returnTypeStart = funcName_start;
				char before, after;
				while (returnTypeStart > 0 && (fileCode[returnTypeStart] != '\n' && fileCode[returnTypeStart] != '}' && fileCode[returnTypeStart] != ';'))
				{
					returnTypeStart--;
					after = fileCode[returnTypeStart];
				}

				fileCode.replace(returnTypeStart, char_position - returnTypeStart + 1, "");
				break;
			}
		}
	}
}

void FillFunctionBody(std::string fileName, std::string functionName, std::string Result, std::string replaceName){
	//std::fstream outFile;
	/*for (int i = 0; i < taskifiedFunctions->size(); i++){
		ASTContext::TaskifyStruct curr_func = (*taskifiedFunctions)[i];
		std::string fileName = curr_func.outFunctionName + ".hpp";*/
		//outFile.open(fileName, std::ios_base::app);
		
		//open file and read all code
		std::ifstream fileIn(fileName);
		std::string code;
		std::string line;
		while (std::getline(fileIn, line))
			code += line + "\n";
		fileIn.close();

		//replace finest function
		replace(code, replaceName/*"FINEST_LEVEL_BODY;"*/, retrieveFunctionBody(Result, functionName/*curr_func.finestFunctionName*/));

		//write code to the file
		std::ofstream fileOut(fileName);
		fileOut << code;
		fileOut.close();
	//}
}

bool PrepareMainFunction(std::string &sourceCode, std::vector<ASTContext::TaskifyStruct> &taskifiedFunctions)
{
	// find position of main
	int start_main = sourceCode.find(" main(");
	if (start_main == -1)
		return false;

	//insert fw_start and fw_end
	int posOfFirstBracket = sourceCode.find_first_of('{', start_main);
	sourceCode = sourceCode.insert(posOfFirstBracket + 1, "\nfw_start();");
	int posOfLastBracket = sourceCode.find_last_of('}');
	sourceCode = sourceCode.insert(posOfLastBracket, "fw_finish();\n");

	//change function call names
	for (int i = 0; i < taskifiedFunctions.size(); i++){
		std::string newFuncName = taskifiedFunctions[i].outFunctionName;
		std::string oldFuncName = taskifiedFunctions[i].taskifiedFunctionName;

		//if the have the same name, nothing has to be changed
		if (newFuncName == oldFuncName)
			continue;

		int current_pos = 0;
		while (current_pos = sourceCode.substr(current_pos).find(oldFuncName))//if functionname is found
		{
			//is the found functionname is not a function call. break
			if (!isCompleteFunctionName(sourceCode, current_pos, oldFuncName.length())){
				//move 1 step to ignore this name in the future
				current_pos++; 
			}
			else{ 
				//it's a function call. rename it
				sourceCode.replace(current_pos, oldFuncName.length(), newFuncName);
				break;
			}
		}
	}

	return true;
}

static const clang::FileEntry * getFileEntryForDecl(const clang::Decl * decl, clang::SourceManager * sourceManager)
{
	if (!decl || !sourceManager) {
		return 0;
	}
	clang::SourceLocation sLoc = decl->getLocation();
	clang::FileID fileID = sourceManager->getFileID(sLoc);
	return sourceManager->getFileEntryForID(fileID);
}

static const char * getFileNameForDecl(const clang::Decl * decl, clang::SourceManager * sourceManager)
{
	const clang::FileEntry * fileEntry = getFileEntryForDecl(decl, sourceManager);
	if (!fileEntry) {
		return 0;
	}
	return fileEntry->getName();
}

bool FrontendAction::Execute() 
{
	CompilerInstance &CI = getCompilerInstance();

	if (CI.hasFrontendTimer()) {
		llvm::TimeRegion Timer(CI.getFrontendTimer());
		ExecuteAction();
	}
	else ExecuteAction();

	//TASKIFY
	bool hasMainFunctionBeenTreated = false;
	/*std::string Result;
	llvm::raw_string_ostream Out(Result);
	CI.getASTContext().getTranslationUnitDecl()->print(Out);*/

	//all input files
	std::vector<FrontendInputFile, std::allocator<FrontendInputFile>> files = CI.getFrontendOpts().Inputs;
	for (int i = 0; i < files.size(); i++)
	{
		StringRef name = files[i].getFile();
		std::string xlat_name = name.str();
		bool new_name = replace(xlat_name, ".cpp", "_xlat.cpp");
		assert(new_name == true);

		// create the _xlat.cpp
		std::ofstream outFile;
		outFile.open(xlat_name);

		// close file
		outFile.close();

		//save the xlat file name
		CI.getASTContext().getXlatFiles()[name.str()] = xlat_name;
	}

	//View file content
	std::map<std::string, std::string> xlat_files_copy = CI.getASTContext().getXlatFiles();

	DeclContext::decl_iterator iter_start = CI.getASTContext().getTranslationUnitDecl()->decls_begin();
	for (; iter_start != CI.getASTContext().getTranslationUnitDecl()->decls_end(); iter_start++)
	{
		//get the filename of the current decl
		clang::SourceLocation sLoc = iter_start->getLocation();
		clang::FileID fileID = CI.getASTContext().getSourceManager().getFileID(sLoc);

		if (fileID.getFileID() == 0)
			continue;

		StringRef fileName = CI.getASTContext().getSourceManager().getFilename(sLoc);
		std::string sourceCode = CI.getASTContext().getSourceManager().getBufferData(fileID).str();

		//check if file is one of the xlat files
		std::map<std::string, std::string>::iterator it = xlat_files_copy.find(fileName.str());
	
		// check if it exists
		if (it == xlat_files_copy.end())
			continue;

		//remove all pragmas from current file
		std::vector<ASTContext::TaskifyStruct> *taskifyFunctions = CI.getASTContext().getTaskifiedFunctions();
		for (int i = 0; i < taskifyFunctions->size(); i++)
		{
			ASTContext::TaskifyStruct curr_func = (*taskifyFunctions)[i];
			if (curr_func.fileName != fileName.str())
				continue;

			std::string finestName = curr_func.finestFunctionName;
			std::string taskifyFunctionName = curr_func.taskifiedFunctionName;

			removeEntireFunction(sourceCode, finestName);
			removeEntireFunction(sourceCode, taskifyFunctionName);

			// remove the pragma line
			int startpos = sourceCode.find("#pragma taskify");
			if (startpos != -1)
			{
				int pragmaLength = sourceCode.find_first_of('\n', startpos) - startpos;
				sourceCode.replace(startpos, pragmaLength + 1, "");
			}

			// insert includes of each taskified file if we are in the main file
			if (sourceCode.find(" main(") != -1 || sourceCode.find(" main ") != -1){
				std::string includeName = "#include \"" + curr_func.outFunctionName + ".hpp\"\n";
				sourceCode.insert(0, includeName);
			}
		}	

		//change the main
		if (hasMainFunctionBeenTreated == false){
			hasMainFunctionBeenTreated = PrepareMainFunction(sourceCode, *taskifyFunctions);
		}

		std::string xlat_name = it->second;	// get xlat filename
		std::ofstream outFile;
		outFile.open(xlat_name, std::ios::app);
		
		// add includes here
		std::map<std::string, std::vector<std::string>> includesPerFile = CI.getASTContext().getIncludedFiles();
		std::map<std::string, std::vector<std::string>>::iterator includesFile = includesPerFile.find(fileName.str());
		// check if it exists
		if (includesFile != includesPerFile.end())
		{
			for (int i = 0; i < includesFile->second.size(); i++){
				std::string name = includesFile->second[i];
				replace(name, "./", "");
				if (name.find("framework.hpp") != -1)
					continue;

				std::string includeName = "#include \"" + name + "\"\n";
				sourceCode.insert(0, includeName);
			}
		}

		// put this include as first line
		std::string frameworkName = "#include \"framework.hpp\"";
		int fram = sourceCode.find(frameworkName);
		if (fram != -1)
		{
			sourceCode.replace(fram, frameworkName.length(), "");
			sourceCode.insert(0, frameworkName + "\n");
		}

		// rest of the codde
		outFile << sourceCode;

		// close file
		outFile.close();

		//remove file from list
		xlat_files_copy.erase(it);
	}

  std::vector<ASTContext::TaskifyStruct> *taskifiedFunctions = this->Instance->getASTContext().getTaskifiedFunctions();

  //TASK ALSO
  FrontendOptions opt = CI.getFrontendOpts();
  //CI.getPreprocessor().getPreprocessingRecord()->getSourceManager().getIncludeLoc();

  //Create output files
  std::vector<std::string> includes; 
  includes.push_back("framework");
  for (int i = 0; i < taskifiedFunctions->size(); i++)
  {
	  ASTContext::TaskifyStruct curr_func = (*taskifiedFunctions)[i];
	  std::string fileName = curr_func.outFunctionName + ".hpp";

	  //get code from file
	  std::ifstream fileIn(curr_func.fileName);
	  std::string code;
	  std::string line;
	  while (std::getline(fileIn, line))
		  code += line + "\n";
	  fileIn.close();

	  CreateOutputFile(curr_func.outFunctionName, fileName, curr_func.taskified_function_params);
	  FillFunctionBody(fileName, curr_func.finestFunctionName, code, "FINEST_LEVEL_BODY;");
	  FillFunctionBody(fileName, curr_func.outFunctionName, code, "ALGORITHM_BODY;");

	  //add all outnames as includes
	  includes.push_back(curr_func.outFunctionName);
  }

  //retrieve and modify main function
  /*
  std::string mainFunc;
  if (taskifiedFunctions->size() > 0){
	  // edit the main body
	  mainFunc = CI.getASTContext().getMainFunctionBody();
	  //mainFunc = PrepareMainFunction(mainFunc, taskifiedFunctions);

	  //Get sourceFileName<----------------------------------------------???
	  std::string fileName = "add_xlat.cpp";

	  //Get include directives from source files<----------------------------------------------???
	  includes.push_back("??");

	  //create one translated file for each source file
	  CreateTranslatedFile(fileName, includes, mainFunc);
  }*/
  
  // If we are supposed to rebuild the global module index, do so now unless
  // there were any module-build failures.
  if (CI.shouldBuildGlobalModuleIndex() && CI.hasFileManager() &&
      CI.hasPreprocessor()) {
    GlobalModuleIndex::writeIndex(
      CI.getFileManager(),
      CI.getPreprocessor().getHeaderSearchInfo().getModuleCachePath());
  }

  return true;
}

void FrontendAction::EndSourceFile() {
  CompilerInstance &CI = getCompilerInstance();

  // Inform the diagnostic client we are done with this source file.
  CI.getDiagnosticClient().EndSourceFile();

  // Inform the preprocessor we are done.
  if (CI.hasPreprocessor())
    CI.getPreprocessor().EndSourceFile();

  // Finalize the action.
  EndSourceFileAction();

  // Sema references the ast consumer, so reset sema first.
  //
  // FIXME: There is more per-file stuff we could just drop here?
  bool DisableFree = CI.getFrontendOpts().DisableFree;
  if (DisableFree) {
    if (!isCurrentFileAST()) {
      CI.resetAndLeakSema();
      CI.resetAndLeakASTContext();
    }
    BuryPointer(CI.takeASTConsumer().get());
  } else {
    if (!isCurrentFileAST()) {
      CI.setSema(nullptr);
      CI.setASTContext(nullptr);
    }
    CI.setASTConsumer(nullptr);
  }

  if (CI.getFrontendOpts().ShowStats) {
    llvm::errs() << "\nSTATISTICS FOR '" << getCurrentFile() << "':\n";
    CI.getPreprocessor().PrintStats();
    CI.getPreprocessor().getIdentifierTable().PrintStats();
    CI.getPreprocessor().getHeaderSearchInfo().PrintStats();
    CI.getSourceManager().PrintStats();
    llvm::errs() << "\n";
  }

  // Cleanup the output streams, and erase the output files if instructed by the
  // FrontendAction.
  CI.clearOutputFiles(/*EraseFiles=*/shouldEraseOutputFiles());

  // FIXME: Only do this if DisableFree is set.
  if (isCurrentFileAST()) {
    CI.resetAndLeakSema();
    CI.resetAndLeakASTContext();
    CI.resetAndLeakPreprocessor();
    CI.resetAndLeakSourceManager();
    CI.resetAndLeakFileManager();
  }

  setCompilerInstance(nullptr);
  setCurrentInput(FrontendInputFile());
}

bool FrontendAction::shouldEraseOutputFiles() {
  return getCompilerInstance().getDiagnostics().hasErrorOccurred();
}

//===----------------------------------------------------------------------===//
// Utility Actions
//===----------------------------------------------------------------------===//

void ASTFrontendAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  if (!CI.hasPreprocessor())
    return;

  // FIXME: Move the truncation aspect of this into Sema, we delayed this till
  // here so the source manager would be initialized.
  if (hasCodeCompletionSupport() &&
      !CI.getFrontendOpts().CodeCompletionAt.FileName.empty())
    CI.createCodeCompletionConsumer();

  // Use a code completion consumer?
  CodeCompleteConsumer *CompletionConsumer = nullptr;
  if (CI.hasCodeCompletionConsumer())
    CompletionConsumer = &CI.getCodeCompletionConsumer();

  if (!CI.hasSema())
    CI.createSema(getTranslationUnitKind(), CompletionConsumer);

  //TASKIFY CHANGE
  //ParseAST(CI.getSema(), CI.getFrontendOpts().ShowStats, CI.getFrontendOpts().SkipFunctionBodies);
  ParseAST(CI.getASTContext(), CI.getSema(), CI.getFrontendOpts().ShowStats,
           CI.getFrontendOpts().SkipFunctionBodies);
}

void PluginASTAction::anchor() { }

std::unique_ptr<ASTConsumer>
PreprocessorFrontendAction::CreateASTConsumer(CompilerInstance &CI,
                                              StringRef InFile) {
  llvm_unreachable("Invalid CreateASTConsumer on preprocessor action!");
}

std::unique_ptr<ASTConsumer>
WrapperFrontendAction::CreateASTConsumer(CompilerInstance &CI,
                                         StringRef InFile) {
  return WrappedAction->CreateASTConsumer(CI, InFile);
}
bool WrapperFrontendAction::BeginInvocation(CompilerInstance &CI) {
  return WrappedAction->BeginInvocation(CI);
}
bool WrapperFrontendAction::BeginSourceFileAction(CompilerInstance &CI,
                                                  StringRef Filename) {
  WrappedAction->setCurrentInput(getCurrentInput());
  WrappedAction->setCompilerInstance(&CI);
  return WrappedAction->BeginSourceFileAction(CI, Filename);
}
void WrapperFrontendAction::ExecuteAction() {
  WrappedAction->ExecuteAction();
}
void WrapperFrontendAction::EndSourceFileAction() {
  WrappedAction->EndSourceFileAction();
}

bool WrapperFrontendAction::usesPreprocessorOnly() const {
  return WrappedAction->usesPreprocessorOnly();
}
TranslationUnitKind WrapperFrontendAction::getTranslationUnitKind() {
  return WrappedAction->getTranslationUnitKind();
}
bool WrapperFrontendAction::hasPCHSupport() const {
  return WrappedAction->hasPCHSupport();
}
bool WrapperFrontendAction::hasASTFileSupport() const {
  return WrappedAction->hasASTFileSupport();
}
bool WrapperFrontendAction::hasIRSupport() const {
  return WrappedAction->hasIRSupport();
}
bool WrapperFrontendAction::hasCodeCompletionSupport() const {
  return WrappedAction->hasCodeCompletionSupport();
}

WrapperFrontendAction::WrapperFrontendAction(FrontendAction *WrappedAction)
  : WrappedAction(WrappedAction) {}

