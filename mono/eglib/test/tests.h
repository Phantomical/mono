#include "test.h"

DEFINE_TEST_GROUP_INIT_H(fake_tests_init);
DEFINE_TEST_GROUP_INIT_H(path_tests_init);
DEFINE_TEST_GROUP_INIT_H(spawn_tests_init);
DEFINE_TEST_GROUP_INIT_H(timer_tests_init);
DEFINE_TEST_GROUP_INIT_H(file_tests_init);
DEFINE_TEST_GROUP_INIT_H(dir_tests_init);
DEFINE_TEST_GROUP_INIT_H(module_tests_init);

const
static Group test_groups [] = {
	{"fake",      fake_tests_init},
	{"path",      path_tests_init},
#if !DISABLE_PROCESS_TESTS
	{"spawn",     spawn_tests_init},
	{"module",    module_tests_init},
#endif
#if !DISABLE_FILESYSTEM_TESTS
	{"file",      file_tests_init},
#endif
	{"timer",     timer_tests_init},
	{"dir",       dir_tests_init},
	{NULL, NULL}
};
