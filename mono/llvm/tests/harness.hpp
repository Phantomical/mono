/*
 * Test scaffolding for mono/llvm/method-to-llvm.
 *
 * The translator needs a real assembly to work from: it resolves field, method and
 * type tokens through mono's metadata, and lays classes out to read field offsets.
 * So the tests boot a runtime, load an assembly ilasm built from a `.il` file next
 * to them, and hand the resulting MonoMethod to method_to_llvm ().
 */

#ifndef MONO_LLVM_TESTS_HARNESS_HPP
#define MONO_LLVM_TESTS_HARNESS_HPP

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

typedef struct _MonoImage MonoImage;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
class LLVMContext;
class Module;
}

namespace mono {
namespace test {

/// Start the runtime, once per process. Safe to call repeatedly.
void init_runtime ();

/// The image of `<name>.dll`, built from `<name>.il` in this directory.
///
/// Aborts rather than returning null: a missing corpus is a build failure, not a
/// property of the translator, and every test in the file would report it.
MonoImage *load_image (const std::string &name);

/// What llvm::verifyFunction says about FUNCTION, empty if it is well formed.
std::string verify_function (llvm::Function &function);

/// What came back from one call to method_to_llvm ().
///
/// The module owns the function and the context owns the module, so all three
/// travel together; `function` is null exactly when the translator refused, and
/// `error` carries the refusal message in that case.
struct Translation {
	std::unique_ptr<llvm::LLVMContext> context;
	std::unique_ptr<llvm::Module> module;
	llvm::Function *function = nullptr;
	std::string error;
	/// What llvm::verifyFunction said, checked by TranslatorTest::TearDown.
	std::string verifier_error;

	/// The function's IR as text, for assertions about what was emitted.
	std::string text () const;

	/// How many times NEEDLE appears in text ().
	size_t count (const std::string &needle) const;
};

/*
 * A test that translates methods and is failed if any of them produced IR the
 * verifier rejects.
 *
 * The verifier check hangs off the fixture rather than off the assertions in
 * each test so that it cannot be left out: every translation this hands back has
 * already been through llvm::verifyFunction, and a broken one fails the test
 * whatever else the test went on to assert.
 */
class TranslatorTest : public ::testing::Test {
public:
	static void SetUpTestSuite () { init_runtime (); }

protected:
	/// Translate `<image>.dll`'s METHOD, named as ilasm spells it: "Type:Method".
	const Translation &translate (const std::string &image, const std::string &method);

	void TearDown () override;

private:
	std::vector<std::unique_ptr<Translation>> translations;
};

} // namespace test
} // namespace mono

#endif
