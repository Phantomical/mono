#include "method-symbols.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <cstdio>
#include <optional>

using namespace llvm;

namespace mono {

namespace {

/// The MonoMethod a marked declaration stands for, as `%p`.
constexpr StringRef method_attribute = "mono-method";
/// Which of that method's entries it wants, as one of the words below.
constexpr StringRef method_entry_attribute = "mono-method-entry";

constexpr StringRef entry_body = "body";
constexpr StringRef entry_legacy = "legacy";
constexpr StringRef entry_unbox = "unbox";

StringRef
word_for (Entry entry)
{
	switch (entry) {
	case Entry::body:
		return entry_body;
	case Entry::legacy:
		return entry_legacy;
	case Entry::unbox:
		return entry_unbox;
	}
	return entry_body;
}

std::optional<Entry>
entry_for (StringRef word)
{
	if (word == entry_body)
		return Entry::body;
	if (word == entry_legacy)
		return Entry::legacy;
	if (word == entry_unbox)
		return Entry::unbox;
	return std::nullopt;
}

/// The marker on VALUE, or nothing if it carries none.
std::optional<std::pair<MonoMethod *, Entry>>
marker_of (GlobalValue &value)
{
	StringRef pointer;
	StringRef word;

	if (auto *fn = dyn_cast<Function> (&value)) {
		if (!fn->hasFnAttribute (method_attribute))
			return std::nullopt;
		pointer = fn->getFnAttribute (method_attribute).getValueAsString ();
		word = fn->getFnAttribute (method_entry_attribute).getValueAsString ();
	} else if (auto *global = dyn_cast<GlobalVariable> (&value)) {
		if (!global->hasAttribute (method_attribute))
			return std::nullopt;
		pointer = global->getAttribute (method_attribute).getValueAsString ();
		word = global->getAttribute (method_entry_attribute).getValueAsString ();
	} else {
		return std::nullopt;
	}

	uintptr_t address = 0;
	std::optional<Entry> entry = entry_for (word);

	if (pointer.getAsInteger (0, address) || address == 0 || !entry)
		return std::nullopt;
	return std::make_pair (reinterpret_cast<MonoMethod *> (address), *entry);
}

} // namespace

void
mark_method_entry (GlobalValue &value, MonoMethod *method, Entry entry)
{
	char printed[32];

	snprintf (printed, sizeof (printed), "%p", (void *) method);

	if (auto *fn = dyn_cast<Function> (&value)) {
		fn->addFnAttr (method_attribute, printed);
		fn->addFnAttr (method_entry_attribute, word_for (entry));
	} else if (auto *global = dyn_cast<GlobalVariable> (&value)) {
		global->addAttribute (method_attribute, printed);
		global->addAttribute (method_entry_attribute, word_for (entry));
	}
}

/*
 * Carry RENAMES through the attributes that hold a symbol name rather than a
 * reference to it - `mono-builtin-target` is one, and it is how a builtin
 * declaration says which method it stands for. An IR use moves with a rename on
 * its own; a name written into a string does not, and the pass that reads it
 * back would go looking for a function that no longer answers to it.
 */
void
follow_renames (Module &m, const StringMap<std::string> &renames)
{
	if (renames.empty ())
		return;

	for (Function &fn : m.functions ()) {
		AttributeSet attrs = fn.getAttributes ().getFnAttrs ();

		for (Attribute attr : attrs) {
			if (!attr.isStringAttribute ())
				continue;

			auto renamed = renames.find (attr.getValueAsString ());

			if (renamed == renames.end ())
				continue;

			StringRef kind = attr.getKindAsString ();

			fn.removeFnAttr (kind);
			fn.addFnAttr (kind, renamed->second);
		}
	}
}

Error
bind_method_symbols (Module &m,
                     function_ref<Expected<std::string> (MonoMethod *, Entry)> name_of)
{
	SmallVector<GlobalValue *, 16> marked;

	for (Function &fn : m.functions ())
		marked.push_back (&fn);
	for (GlobalVariable &global : m.globals ())
		marked.push_back (&global);

	StringMap<std::string> renames;

	for (GlobalValue *value : marked) {
		/*
		 * Only a declaration stands for something the engine publishes; a
		 * definition is the thing itself and keeps whatever it was called.
		 * That matters here rather than being a nicety: a thunk built to a
		 * declaration's shape is given its attributes, marker and all, so
		 * without this the dispatcher would be renamed to the method it
		 * dispatches for and the name it was compiled under would not exist.
		 */
		if (!value->isDeclaration ())
			continue;

		std::optional<std::pair<MonoMethod *, Entry>> marker = marker_of (*value);

		if (!marker)
			continue;

		Expected<std::string> name = name_of (marker->first, marker->second);

		if (!name)
			return name.takeError ();
		if (value->getName () == *name)
			continue;

		renames[value->getName ()] = *name;

		/*
		 * setName () uniques against whatever already answers to the name
		 * rather than failing, which would leave a reference to a symbol
		 * nothing defines. Two references to one entry have to become the
		 * same value instead.
		 */
		if (GlobalValue *existing = m.getNamedValue (*name)) {
			value->replaceAllUsesWith (existing);
			value->eraseFromParent ();
			continue;
		}

		value->setName (*name);
	}

	follow_renames (m, renames);
	return Error::success ();
}

} // namespace mono
