#include "runtime-error.hpp"

namespace mono {

char RuntimeError::ID = 0;

RuntimeError::RuntimeError (MonoError *error)
{
	mono_error_move (&this->error, error);
}

RuntimeError::~RuntimeError ()
{
	mono_error_cleanup (&error);
}

void
RuntimeError::move_to (MonoError *dest)
{
	mono_error_move (dest, &error);
}

uint16_t
RuntimeError::code () const
{
	return mono_error_get_error_code (&error);
}

void
RuntimeError::log (llvm::raw_ostream &os) const
{
	const char *message = mono_error_get_message (&error);

	os << (message != nullptr ? message : "no error");
}

std::error_code
RuntimeError::convertToErrorCode () const
{
	return llvm::inconvertibleErrorCode ();
}

llvm::Error
runtime_error (MonoError *error)
{
	if (is_ok (error))
		return llvm::Error::success ();

	return llvm::make_error<RuntimeError> (error);
}

} // namespace mono
