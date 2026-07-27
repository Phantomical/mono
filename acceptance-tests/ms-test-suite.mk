check-ms-test-suite: $(CLASS)/nunitlite.dll
	@if test -d $(MSTESTSUITE_PATH)/conformance; then \
		$(MAKE) -C $(MSTESTSUITE_PATH)/conformance build MCS="$(MCS) -debug:portable -t:library -warn:1 -r:$(CLASS)/nunitlite.dll" && \
		$(MAKE) -C $(MSTESTSUITE_PATH)/systemruntimebringup build MCS="$(MCS) -debug:portable -t:library -warn:1 -r:$(CLASS)/nunitlite.dll" && \
		$(MAKE) -C $(MSTESTSUITE_PATH)/System.Linq.Expressions build MCS="$(MCS) -debug:portable -t:library -warn:1 -r:$(CLASS)/nunitlite.dll" && \
		$(MAKE) -C $(MSTESTSUITE_PATH)/conformance run NUNIT-CONSOLE="$(RUNTIME) $(CLASS)/nunit-lite-console.exe -exclude=MonoBug,BadTest -format:nunit2" NUNIT_XML_RESULT="-result:$(abs_top_builddir)/acceptance-tests/TestResult-ms-test-suite-conformance.xml" || EXIT_CODE=1; \
		$(MAKE) -C $(MSTESTSUITE_PATH)/systemruntimebringup run NUNIT-CONSOLE="$(RUNTIME) $(CLASS)/nunit-lite-console.exe -exclude=MonoBug,BadTest -format:nunit2" NUNIT_XML_RESULT="-result:$(abs_top_builddir)/acceptance-tests/TestResult-ms-test-suite-systemruntimebringup.xml" || EXIT_CODE=1; \
		$(MAKE) -C $(MSTESTSUITE_PATH)/System.Linq.Expressions run NUNIT-CONSOLE="$(RUNTIME) $(CLASS)/nunit-lite-console.exe -exclude=MonoBug,BadTest -format:nunit2" NUNIT_XML_RESULT="-result:$(abs_top_builddir)/acceptance-tests/TestResult-ms-test-suite-systemlinqexpressions.xml" || EXIT_CODE=1; \
		exit $$EXIT_CODE; \
	else \
		echo "*** [ms-test-suite] Not checked out. It is a submodule marked 'update = none' because it is Xamarin-internal; if you have access, run 'git submodule update --init --checkout acceptance-tests/external/ms-test-suite'. Skipping."; \
	fi

$(CLASS)/nunitlite.dll:
	$(MAKE) -C $(mcs_topdir)/tools/nunit-lite
