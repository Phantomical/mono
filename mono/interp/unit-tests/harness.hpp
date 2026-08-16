/*
 * Test scaffolding for the interpreter's transform.
 *
 * A case here asks what bytecode the transform produces for one method. That
 * needs real metadata, so the tests boot a runtime, load an assembly ilasm built
 * from a `.il` file under il/, and run the transform over a method named in it.
 * What comes back is an instruction list, which the case reads.
 */

#ifndef __MONO_INTERP_TESTS_HARNESS_HPP__
#define __MONO_INTERP_TESTS_HARNESS_HPP__

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mono/interp/transform.h"

namespace mono {
namespace test {

/// Start the runtime, once per process. Safe to call repeatedly.
void init_runtime ();

/// Whether this build has the managed corpus these tests run against.
bool have_corpus ();

/*
 * Skip the case unless the corpus is there.  A build configured with
 * -DMONO_ENABLE_MCS_BUILD=OFF has no class library to boot a runtime on and no
 * il/ image to load, and a case that cannot run has to say so as a skip rather
 * than vanish from the list.
 */
#define MONO_SKIP_WITHOUT_CORPUS()					\
	do {								\
		if (!mono::test::have_corpus ())			\
			GTEST_SKIP () << "no managed corpus in this build"; \
	} while (0)

/// The transform's output for one method, and the arena it sits in.
///
/// Everything read out of it - a Code, an InterpInst * - points into a mempool
/// this owns, so it has to outlive them.
class Transform {
public:
	/// Transforms `method` of il/<image>.il. The method is named the way
	/// mono_method_desc_new () spells one: "Class:name".
	///
	/// A non-zero `verbose_level` makes the transform print what it does, the
	/// same way MONO_VERBOSE_METHOD does for a whole run.
	Transform (const std::string &image, const std::string &method,
	           int verbose_level = 0);
	~Transform ();

	Transform (const Transform &) = delete;
	Transform &operator= (const Transform &) = delete;

	TransformData *get () { return &td; }

	/// Runs constant propagation over what the transform emitted.
	void cprop ();

private:
	TransformData td;
	InterpMethod rtm;
	MonoMethodHeader *header;
};

/// A transform's instructions in order, with the MINT_NOPs dropped.
class Code {
public:
	explicit Code (Transform &transform);

	/// The opcode names. gtest prints two of these side by side when they
	/// differ, which numbers would not survive.
	const std::vector<std::string> &opcodes () const { return names; }

	/// The instruction at `index`, or null past the end.
	InterpInst *at (size_t index) const;

private:
	std::vector<InterpInst *> instructions;
	std::vector<std::string> names;
};

} // namespace test
} // namespace mono

#endif /* __MONO_INTERP_TESTS_HARNESS_HPP__ */
