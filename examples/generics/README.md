| Test File | Description |
| --- | --- |
| `01_basic_constrained_add.risk` | Tests a single type parameter `T: _Set` instantiated with two different concrete types across separate call sites. |
| `02_type_inference_from_args.risk` | Tests type argument inference directly from argument types, verifying that explicit function call syntax like `f<T>(...)` is not required. |
| `03_multiple_generic_params.risk` | Tests functions configured with more than one independent generic parameter, such as `<T: U: _Set _Set,>`. |
| `04_nested_and_recursive_generic_calls.risk` | Tests a generic function calling another generic function, alongside a generic function calling itself recursively. |
| `05_instance_reuse_dedup.risk` | Verifies that repeat calls using the identical concrete type combination reuse a single monomorphized instance from the `generic_instances` cache rather than generating duplicate C functions. |
| `06_compound_type_pattern_array.risk` | Tests structural matching of a compound type like `Array<i32>` against a generic set member for both constraint checking and parameter binding. |
| `07_nested_generic_set_membership.risk` | Tests transitive membership where a generic set references another generic set's name as a member. |
| `08_error_constraint_violation.risk` | Tests error handling when an argument type unifies correctly but fails the constraint set membership check. |
| `09_error_conflicting_types.risk` | Tests error handling when a single generic parameter appears in multiple argument positions with mismatched concrete types. |
| `10_error_arg_count_mismatch.risk` | Tests error handling when a generic call site provides an incorrect number of arguments. |
| `11_error_unbound_type_param.risk` | Tests error handling when a generic parameter appears only in the return type rather than parameter types, preventing automatic inference. |
| `12_default_args_now_supported.risk` | Validates that default parameter values on a generic function can be omitted at call sites, matching the behavior of ordinary functions. |
| `13_contrast_default_args_ordinary_function.risk` | Serves as a companion test using the exact same default parameter pattern on a non-generic function to confirm consistent behavior across both paths. |
