AC_DEFUN([AX_ARG_ENABLE_WARNINGS],[
  AC_ARG_ENABLE([warnings],[AS_HELP_STRING([--enable-warnings],[Enable compiler warnings.])],[warnings="$enableval"],[warnings="no"])

  AS_IF([test "$warnings" = "yes"], [
    AX_APPEND_COMPILE_FLAGS([-Wall])
    AX_APPEND_COMPILE_FLAGS([-Wbad-function-cast])
    AX_APPEND_COMPILE_FLAGS([-Wcast-align])
    AX_APPEND_COMPILE_FLAGS([-Wcast-function-type])
    AX_APPEND_COMPILE_FLAGS([-Wcast-qual])
    AX_APPEND_COMPILE_FLAGS([-Wclobbered])
    AX_APPEND_COMPILE_FLAGS([-Wduplicated-cond])
    AX_APPEND_COMPILE_FLAGS([-Wempty-body])
    AX_APPEND_COMPILE_FLAGS([-Wenum-conversion])
    AX_APPEND_COMPILE_FLAGS([-Wignored-qualifiers])
    AX_APPEND_COMPILE_FLAGS([-Wlogical-op])
    AX_APPEND_COMPILE_FLAGS([-Wmissing-declarations])
    AX_APPEND_COMPILE_FLAGS([-Wmissing-field-initializers])
    AX_APPEND_COMPILE_FLAGS([-Wmissing-include-dirs])
    AX_APPEND_COMPILE_FLAGS([-Wmissing-parameter-type])
    AX_APPEND_COMPILE_FLAGS([-Wmissing-prototypes])
    AX_APPEND_COMPILE_FLAGS([-Wnested-externs])
    AX_APPEND_COMPILE_FLAGS([-Wold-style-declaration])
    AX_APPEND_COMPILE_FLAGS([-Woverride-init])
    AX_APPEND_COMPILE_FLAGS([-Wpointer-arith])
    AX_APPEND_COMPILE_FLAGS([-Wredundant-decls])
    AX_APPEND_COMPILE_FLAGS([-Wshadow])
    AX_APPEND_COMPILE_FLAGS([-Wshift-negative-value])
    AX_APPEND_COMPILE_FLAGS([-Wsign-compare])
    AX_APPEND_COMPILE_FLAGS([-Wtype-limits])
    AX_APPEND_COMPILE_FLAGS([-Wundef])
    AX_APPEND_COMPILE_FLAGS([-Wuninitialized])
    AX_APPEND_COMPILE_FLAGS([-Wwrite-strings])
  ])
])
