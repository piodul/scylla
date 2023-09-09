use proc_macro::TokenStream;
use quote::{quote, quote_spanned};

/// Turns given function into a Seastar test case.
///
/// Rust has a built-in testing harness, but it is very limited in its
/// configurability. _We don't support tests marked with `#[test]` attribute
/// at all_. All tests should use the `#[seastar::test]` attribute instead.
///
/// No sub-attributes are supported at the moment.
#[proc_macro_attribute]
pub fn test(args: TokenStream, item: TokenStream) -> TokenStream {
    let args = syn::parse_macro_input!(args as proc_macro2::TokenStream);
    let input = syn::parse_macro_input!(item as syn::ItemFn);

    if !args.is_empty() {
        return syn::Error::new_spanned(
            args,
            "Arguments for #[seastar::test] are not supported yet",
        )
        .to_compile_error()
        .into();
    }

    if !input.sig.inputs.empty_or_trailing() {
        return syn::Error::new_spanned(
            input.sig.inputs,
            "A test case must not have any arguments",
        )
        .to_compile_error()
        .into();
    }

    if !matches!(input.sig.output, syn::ReturnType::Default) {
        return syn::Error::new_spanned(
            input.sig.inputs,
            "A test case must not specify a return type",
        )
        .to_compile_error()
        .into();
    }

    // TODO: Check for more weird stuff in the function definitions: extern "C", etc.

    let test = &input;
    let test_ident = &input.sig.ident;
    let test_name = test_ident.to_string();

    let runner_contents = if input.sig.asyncness.is_some() {
        // Convert the future returned from the test function
        // TODO
        todo!("async functions are not supported yet")
    } else {
        // Run the function directly
        quote!(
            #test_ident()
        )
    };

    let output = quote_spanned!(input.sig.ident.span() => {
        #[::seastar::test::ctor]
        #[doc(hidden)]
        #[allow(non_upper_case_globals)]
        static #test_ident: ::cxx::UniquePtr<::seastar::test::RustTest> = {
            #test
            fn runner() -> ::seastar::future::CppVoidFuture {
                #runner_contents
            }
            ::seastar::test::create_rust_test(
                #test_name,
                ::std::file!(),
                ::std::line!(),
                runner,
            )
        }
    });

    output.into()
}
