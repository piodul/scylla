#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2023-present ScyllaDB
#

#
# SPDX-License-Identifier: AGPL-3.0-or-later
#


# Don't bother with this approach for now


from enum import Enum
from typing import List, Union, Dict


_CPP_TYPE_REGISTRY = []


class CppFfiFlavor(Enum):
    OPAQUE = "opaque"
    MOVABLE = "movable"
    BOXED = "boxed"


class CppType(object):
    pass


class CppTemplateArg(CppType):
    def __init__(self, name: str):
        self._name = name


class CppCustomType(CppType):
    def __init__(self, name: str, flavor: CppFfiFlavor = CppFfiFlavor.OPAQUE):
        self._name = name
        self._flavor = flavor
        self._template_args = []
        self._methods = []

        global _CPP_TYPE_REGISTRY
        _CPP_TYPE_REGISTRY.append(self)

    def template(self, args: List[str]):
        self._template_args = args

    def method(self, name: str) -> 'CppMethod':
        method = CppMethod(name)
        self._methods.append(method)
        return method
    
    def bound(self, *args, **kwargs):
        assert not args or not kwargs, "Cannot both pass args and kwargs"
        if args:
            mapping = dict(zip(self.template_args, args))
        else:
            mapping = kwargs
        return CppCustomTypeBound(self, mapping)


# A template, bound to a list of arguments
class CppCustomTypeBound(CppType):
    def __init__(self, template: CppCustomType, mapping: Dict[str, CppType]):
        self._mapping = mapping


class CppMethod(object):
    def __init__(self, name: str):
        self._name = name
        self._return_type = None
        self._arguments = []
    
    def returns(self, typ: CppType) -> 'CppMethod':
        self._return_type = typ
        return self

    def argument(self, name: str, typ: CppType) -> 'CppMethod':
        self._arguments.append((name, typ))
        return self





CxxExceptionPtr = CppCustomType("CxxExceptionPtr", flavor=CppFfiFlavor.MOVABLE)


BoxPromise = CppCustomType("BoxPromise", flavor=CppFfiFlavor.BOXED)
BoxPromise.template(["T"])

BoxFuture = CppCustomType("BoxFuture", flavor=CppFfiFlavor.BOXED)
BoxFuture.template(["T"])

BoxPromise \
    .method("set_value") \
    .argument("value", CppTemplateArg("T"))
BoxPromise \
    .method("set_exception") \
    .argument("eptr", CxxExceptionPtr)
BoxPromise \
    .method("get_future") \
    .returns(BoxFuture.bound(T=CppTemplateArg("T")))

