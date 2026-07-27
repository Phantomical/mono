SYS_REFS = \
	System.dll \
	System.Core.dll \
	System.Data.dll \
	System.Runtime.Serialization.dll \
	System.Xml.dll \
	System.Xml.Linq.dll \
	Mono.Posix.dll

check-profiler-stress:
	@test -d $(BENCHMARKER_PATH)/benchmarks || { \
		echo "*** [benchmarker] checkout missing; run:"; \
		echo "***   git submodule update --init acceptance-tests/external/benchmarker"; \
		exit 1; }
	cd profiler-stress && $(MCS) -debug -define:ARCH_$(arch_target) -target:exe $(addprefix -r:, $(SYS_REFS)) -out:runner.exe @runner.exe.sources
	cd profiler-stress && $(RUNTIME) runner.exe
