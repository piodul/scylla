extern crate proc_macro;
use std::path::PathBuf;
use std::process::{Command, Stdio};

use proc_macro::TokenStream as TokenStream1;
use proc_macro2::TokenStream;
use quote::ToTokens;
use syn::spanned::Spanned;
use syn::{parse_macro_input, parse_quote, parse_quote_spanned, ItemFn, Lit};

// Internal to the `seastar::crate`.
// Calls ./gen.py with given argument.
#[doc(hidden)]
#[proc_macro]
pub fn gen_py(item: TokenStream1) -> TokenStream1 {
    let arg = parse_macro_input!(item as Lit);
    let arg = match arg {
        Lit::Str(s) => s.value(),
        _ => panic!("Invalid literal passed as an argument, only string literals are accepted"),
    };
    let mut path_to_gen_py = PathBuf::new();
    path_to_gen_py.push(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    path_to_gen_py.push("../gen.py");
    let mut path_to_yaml = PathBuf::new();
    path_to_yaml.push(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    path_to_yaml.push(arg);

    let out = Command::new("python3")
        .arg(path_to_gen_py.as_os_str())
        .arg("rust")
        .arg(path_to_yaml)
        .stderr(Stdio::inherit())
        .output()
        .unwrap();

    if !out.status.success() {
        panic!("./gen.py invocation failed: {}", out.status);
    }

    let out = String::from_utf8(out.stdout).unwrap();
    syn::parse_str::<TokenStream>(&out).unwrap().into()
}

/// Translates a Rust `async fn` into a function that spawns a task and returns a `BoxFuture`.
///
/// The main purpose of this macro is to allow writing regular `async fn`s those results can be passed back to C++
/// without problems.
#[proc_macro_attribute]
pub fn taskify(attr: TokenStream1, item: TokenStream1) -> TokenStream1 {
    let mut crate_path: syn::Path = parse_quote!(::seastar);
    let meta_parser = syn::meta::parser(|meta| {
        if meta.path.is_ident("crate") {
            crate_path = meta.value()?.parse()?;
            Ok(())
        } else {
            Err(meta.error("unsupported parameter for the `taskify` macro"))
        }
    });
    parse_macro_input!(attr with meta_parser);

    let ItemFn {
        attrs,
        vis,
        mut sig,
        block,
    } = parse_macro_input!(item as ItemFn);

    if sig.asyncness.is_none() {
        return syn::Error::new_spanned(sig, "The function must be marked as async")
            .into_compile_error()
            .into();
    }

    if sig.abi.is_some() {
        return syn::Error::new_spanned(sig.abi, "The function must not use a non-standard ABI")
            .into_compile_error()
            .into();
    }

    if !sig.generics.params.is_empty() {
        return syn::Error::new_spanned(sig.generics, "Generic functions are not supported")
            .into_compile_error()
            .into();
    }

    if sig.constness.is_some() {
        return syn::Error::new_spanned(sig.constness, "Const functions are not supported")
            .into_compile_error()
            .into();
    }

    // Temove `async` from `async fn` and convert the function to return BoxFuture
    sig.asyncness = None;
    sig.output = match &sig.output {
        syn::ReturnType::Default => {
            parse_quote_spanned!(sig.output.span()=> -> #crate_path::future::BoxFuture<()>)
        }
        syn::ReturnType::Type(_, tt) => {
            parse_quote_spanned!(sig.output.span()=> -> #crate_path::future::BoxFuture<#tt>)
        }
    };

    // The function becomes unsafe to call because the returned future
    // might capture some references, and they need to be kept alive
    // until the task finishes executing.
    sig.unsafety = Some(syn::Token![unsafe](sig.ident.span()));

    quote::quote! {
        #(#attrs)*
        #vis #sig {
            #crate_path::task::spawn_for_cpp_with_any_lifetime(async move #block)
        }
    }
    .into_token_stream()
    .into()
}
