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

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

typedef struct _MonoImage MonoImage;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
class LLVMContext;
class Module;
} // namespace llvm

namespace mono {

class MonoJit;

namespace test {

/// Start the runtime, once per process. Safe to call repeatedly.
void init_runtime ();

/// A MonoJit over one code arena this binary shares.
///
/// Needs a runtime, so call init_runtime () first. The arena is never destroyed
/// and code is never given back, which is what lets a case hold an engine or an
/// address past the end of its own body.
llvm::Expected<std::unique_ptr<MonoJit>> make_jit ();

/// Whether this build has the managed corpus these tests run against.
bool have_corpus ();

/*
 * Skip the case unless the corpus is there.  A build configured with
 * -DMONO_ENABLE_MCS_BUILD=OFF has the backend and nothing managed, so there is
 * no class library to boot a runtime on and no il/ image to translate -- and a
 * case that cannot run has to say so as a skip rather than vanish from the list.
 * Good in a test body and in SetUpTestSuite (), which is where the fixtures that
 * bring the runtime up need it.
 */
#define MONO_SKIP_WITHOUT_CORPUS()					\
	do {								\
		if (!mono::test::have_corpus ())			\
			GTEST_SKIP () << "no managed corpus in this build"; \
	} while (0)

/// The image of `<name>.dll`, built from `<name>.il` in this directory. The one
/// name that is not a fixture is "mscorlib", which is the class library the
/// runtime booted on.
///
/// Aborts rather than returning null: a missing corpus is a build failure, not a
/// property of the translator, and every test in the file would report it.
MonoImage *load_image (const std::string &name);

/// What llvm::verifyFunction says about FUNCTION, empty if it is well formed.
std::string verify_function (llvm::Function &function);

/*
 * The dump name a case gives a body it wants a dump point to print.
 *
 * This binary turns the points on and sets the filter to this name before any
 * case runs, because the dump variables are read once, on the first compile of
 * the process, and booting the runtime already compiles a dozen methods. So a
 * body a case names this way is dumped and no other compile is.
 */
extern const char *const dump_filter;

/// Everything written to fd 1 - where a dump goes with no directory set - while
/// this is alive.
class CapturedStdout {
public:
	CapturedStdout ();
	~CapturedStdout ();

	CapturedStdout (const CapturedStdout &) = delete;
	CapturedStdout &operator= (const CapturedStdout &) = delete;

	/// What has been written so far, with stdout handed back to the process.
	std::string text ();

private:
	void restore ();

	FILE *sink_ = nullptr;
	int saved_ = -1;
};

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

/// Translate `<image>.dll`'s METHOD, named as ilasm spells it ("Type:Method"),
/// and hand the result to the caller. This is the ownership-passing form the
/// jit tests use to move the module into a ThreadSafeModule; TranslatorTest
/// keeps its returned translations alive itself.
std::unique_ptr<Translation> translate_method (const std::string &image,
                                               const std::string &method);

/// Translate METHOD into a module that already holds one, and mark it
/// always-inline, which is what the engine's pre-pass does to a callee whose IL
/// says the fold pays (runtime/trivial-inlines.cpp). The translator declares a
/// callee under a name of its own and finds it again by that name, so a method
/// the module already calls comes back as the body behind that call.
///
/// Returns null when the translator refuses, leaving into unchanged. None of the
/// engine's gates run here: a test says which body it wants folded.
llvm::Function *fold_method_into (Translation &into, const std::string &image,
                                  const std::string &method);

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
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}

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
