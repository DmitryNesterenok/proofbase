include (../tests/tests.pri)

SOURCES += \
    tests/proofcore/main.cpp \
    tests/proofcore/humanizer_test.cpp \
    tests/proofcore/taskchain_test.cpp \
    tests/proofcore/unbounded_taskchain_test.cpp \
    tests/proofcore/objectscache_test.cpp \
    tests/proofcore/settings_test.cpp \
    tests/proofcore/proofobject_test.cpp

RESOURCES += \
    tests/proofcore/tests_resources.qrc