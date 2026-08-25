/*
 * Tests for the module an IR dump point prints (mono/llvm/dump.cpp).
 *
 * What makes the module rather than the function worth a test is that a reader
 * runs `opt` over the text. So the cases below parse the dump back, which is
 * the same question that reader asks.
 */

#include "harness.hpp"

#include "dump.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

class IrDump : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}

protected:
	/// Translates image's method and prints it at the `unopt-ir` point, then
	/// hands back what the point wrote.
	///
	/// folded names a second method of the same image to translate in beside
	/// it and mark always-inline, the way the engine's pre-pass folds a callee.
	std::string dump (const std::string &image, const std::string &method,
	                  const std::string &folded = std::string ())
	{
		std::unique_ptr<Translation> t = translate_method (image, method);

		EXPECT_NE (t->function, nullptr) << t->error;
		if (t->function == nullptr)
			return std::string ();

		if (!folded.empty ())
			EXPECT_NE (fold_method_into (*t, image, folded), nullptr);

		/* As the engine does, so the dump is filed and filtered by method. */
		set_dump_name (*t->function, dump_filter);

		CapturedStdout captured;

		cantFail (dump_body_module (DumpPoint::unopt_ir, *t->module,
		                            t->function->getName (), dump_filter));

		return captured.text ();
	}

	/// Reads text as a module and verifies it, the way an `opt` run over the
	/// same file does. Hands back null and what refused it.
	///
	/// The context is the caller's, because the module it hands back names
	/// types and metadata that belong to one.
	static std::unique_ptr<Module> parsed (const std::string &text,
	                                       LLVMContext &context,
	                                       std::string &complaint)
	{
		SMDiagnostic problem;
		std::unique_ptr<Module> module =
			parseAssemblyString (text, problem, context);
		raw_string_ostream out (complaint);

		if (module == nullptr) {
			problem.print ("dump", out);
			return nullptr;
		}

		if (verifyModule (*module, &out))
			return nullptr;

		return module;
	}
};

/*
 * A method whose body names a runtime helper, a landing pad and the metadata a
 * clause is described by. Printed alone, each of those is a name the text does
 * not define.
 */
TEST_F (IrDump, PrintsAModuleThatParsesOnItsOwn)
{
	std::string text = dump ("eh", "Eh:TryCatch");
	LLVMContext context;
	std::string complaint;

	ASSERT_FALSE (text.empty ());

	std::unique_ptr<Module> module = parsed (text, context, complaint);

	ASSERT_NE (module, nullptr) << complaint << text;

	size_t bodies = 0;

	for (const Function &function : *module)
		if (!function.isDeclaration ())
			++bodies;

	EXPECT_EQ (bodies, 1u) << text;
}

/*
 * A folded copy carries no symbol, so a reader has its body from this dump or
 * not at all.
 */
TEST_F (IrDump, KeepsTheBodyOfAFoldedCallee)
{
	std::string text = dump ("calls", "Calls:CallStatic", "Calls:Helper");
	LLVMContext context;
	std::string complaint;

	ASSERT_FALSE (text.empty ());

	std::unique_ptr<Module> module = parsed (text, context, complaint);

	ASSERT_NE (module, nullptr) << complaint << text;

	size_t defined = 0;

	for (const Function &function : *module)
		if (!function.isDeclaration ())
			++defined;

	EXPECT_EQ (defined, 2u) << text;
}

/*
 * The other members of a batch share the module the dump is taken from, and a
 * dump is about one method.
 */
TEST_F (IrDump, DropsTheBodyOfAnotherMethodInTheModule)
{
	std::unique_ptr<Translation> t = translate_method ("calls", "Calls:CallStatic");

	ASSERT_NE (t->function, nullptr) << t->error;

	/* A batch member: a body of its own, under a symbol of its own. */
	Function *other = fold_method_into (*t, "arith", "Arith:Add");

	ASSERT_NE (other, nullptr);
	other->removeFnAttr (Attribute::AlwaysInline);
	other->setLinkage (GlobalValue::ExternalLinkage);

	std::string other_name = other->getName ().str ();

	/*
	 * What the tier-1 profiling instrumentation gives each body it counts.
	 * An alias must point to a definition, so this is the shape that a
	 * dropped body leaves invalid behind it.
	 */
	GlobalAlias::create (other->getValueType (), other->getAddressSpace (),
	                     GlobalValue::PrivateLinkage, other->getName () + ".local",
	                     other, t->module.get ());

	set_dump_name (*t->function, dump_filter);

	std::string text;
	{
		CapturedStdout captured;

		cantFail (dump_body_module (DumpPoint::unopt_ir, *t->module,
		                            t->function->getName (), dump_filter));
		text = captured.text ();
	}

	LLVMContext context;
	std::string complaint;
	std::unique_ptr<Module> module = parsed (text, context, complaint);

	ASSERT_NE (module, nullptr) << complaint << text;

	const Function *dropped = module->getFunction (other_name);

	EXPECT_TRUE (dropped == nullptr || dropped->isDeclaration ()) << text;
}

} // namespace
} // namespace test
} // namespace mono
