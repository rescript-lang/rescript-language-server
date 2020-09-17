
open Belt.Result;

let tryReadCmi = cmi =>
  switch (Cmi_format.read_cmi(cmi)) {
  | exception err => Error("Invalid cmi format " ++ cmi ++ " - probably wrong ocaml version, expected " ++ Config.version ++ " : " ++ Printexc.to_string(err))
  | x => Ok(x)
  };

let tryReadCmt = cmt => {
  if (!Util.Files.exists(cmt)) {
    Error("Cmt file does not exist " ++ cmt)
  } else {
    switch (Cmt_format.read_cmt(cmt)) {
    | exception Cmi_format.Error(err) =>
      Error("Failed to load " ++ cmt ++ " as a cmt w/ ocaml version " ++
      "406" ++
      ", error: " ++ {
        Cmi_format.report_error(Format.str_formatter, err);
        Format.flush_str_formatter();
      })
    | exception err => Error("Invalid cmt format " ++ cmt ++ " - probably wrong ocaml version, expected " ++ Config.version ++ " : " ++ Printexc.to_string(err))
    | x => Ok(x)
    };
  }
};

/** TODO move to the Process_ stuff */
let rec dig = (typ) =>
  switch typ.Types.desc {
  | Types.Tlink(inner) => dig(inner)
  | Types.Tsubst(inner) => dig(inner)
  | Types.Tpoly(inner, _) => dig(inner)
  | _ => typ
  };

let digConstructor = (expr) => {
  let expr = dig(expr);
  switch (expr.desc) {
  | Tconstr(path, args, _memo) => Some((path, args))
  | _ => None
  };
};

let rec variableKind = (t) =>
  switch t.Types.desc {
  | Tlink(t) => variableKind(t)
  | Tsubst(t) => variableKind(t)
  | Tarrow(_) => SharedTypes.Function
  | Ttuple(_) => Array
  | Tconstr(_) => Variable
  | Tobject(_) => Object
  | Tnil => Null
  | Tvariant(_) => EnumMember
  | Tpoly(_) => EnumMember
  | Tpackage(_) => Module
  | _ => Variable
  };

let typeKind = (t) =>
  switch t.Types.type_kind {
  | Type_open
  | Type_abstract => SharedTypes.TypeParameter
  | Type_record(_) => Interface
  | Type_variant(_) => Enum
  };

let rec getFnArgs = t => {
  switch (dig(t).desc) {
    | Tarrow(label, arg, res, _) =>
      let (args, res) = getFnArgs(res);
      ([(label, arg), ...args], res)
    | _ => ([], t)
  }
};

/* Unfortunately we need to depend on previous Compiler_libs_* versions starting
   with 4.07, otherwise we can't construct an ident with a custom stamp.
 */


/* HACK(jared): They removed all way for me to produce an "Ident.t" with the correct stamp.
   They forced my hand.
*/

let rec asSimpleType = t => {
  open SharedTypes;
  switch (dig(t).desc) {
    | Tvar(None) => SimpleType.AnonVariable
    | Tvar(Some(text)) => SimpleType.Variable(text)
    | Tarrow(label, arg, res, _) =>
      let (args, res) = getFnArgs(res);
      let args = [(label, arg), ...args];
      let args = args->Belt.List.map(((label, arg)) => (
        /* label */
        switch label {
          | Nolabel => None
          | Labelled(x) => Some(x)
          | Optional(x) => Some(x)
        }
        , asSimpleType(arg)));
      SimpleType.Fn(args, asSimpleType(res))
    | Ttuple(items) =>
      SimpleType.Tuple(items->Belt.List.map(asSimpleType))
    | Tconstr(path, args, _) =>
      SimpleType.Reference(path, args->Belt.List.map(asSimpleType))
    | Tvariant({row_fields, row_more: _, row_closed, row_fixed: _, row_name: _}) =>
      SimpleType.RowVariant(
        row_fields->Belt.List.map(((label, field)) => switch field {
          | Reither(_, [arg], _, _) => (label, Some(asSimpleType(arg)))
          | _ => (label, None)
        }),
        row_closed
      )
    | _ => SimpleType.Other
  }
};

let asSimpleDeclaration = (name, t) => {
  open SharedTypes;
  {
    SimpleType.name,
    variables: t.Types.type_params->Belt.List.map(param => {
      asSimpleType(param)
    }),
    body: switch (t.type_kind, t.type_manifest) {
      | (Type_open, _) => Open
      | (Type_abstract, None) => Abstract
      | (Type_abstract, Some(expr)) => Expr(asSimpleType(expr))
      | (Type_record(labels, _), _) => Record(
        labels->Belt.List.map(({ld_id, ld_type}) => (
          Ident.name(ld_id),
          asSimpleType(ld_type)
        ))
      )
      | (Type_variant(constructors), _) => Variant(
        constructors->Belt.List.map(({cd_id, cd_args, cd_res}) => (
          Ident.name(cd_id),
          switch (cd_args) {
            | Cstr_tuple(args) =>
              args->Belt.List.map(asSimpleType)
            | Cstr_record(_) => []
          },
          switch (cd_res) {
          | None => None
          | Some(arg) => Some(asSimpleType(arg))
          }
        ))
      )
    }
  }
};

let migrateAttributes = t => {
  t.Types.type_attributes
};

let makeDeclaration = t => {
  SharedTypes.declToString: name =>
PrintType.default.decl(PrintType.default, name, name, t) |> PrintType.prettyString,
  declarationKind: typeKind(t),
  asSimpleDeclaration: name => asSimpleDeclaration(name, t),
  migrateAttributes: () => migrateAttributes(t),
}

let labelToString = label => switch label {
  | Asttypes.Nolabel => ""
  | Optional(label) | Labelled(label) => label
};

let rec makeFlexible = t => {
  SharedTypes.toString: () => {
    PrintType.default.expr(PrintType.default, t)
    |> PrintType.prettyString(~width=40)
  },
  variableKind: variableKind(t),
  getConstructorPath: () => switch (digConstructor(t)) {
    | None => None
    | Some((path, args)) =>
      Some((path, args |> List.map(makeFlexible)))
  },
  getArguments: () => {
      loop(t)
  },
  asSimpleType: () => asSimpleType(t)
}

and loop = t => switch (t.Types.desc) {
  | Types.Tsubst(t)
  | Tlink(t) => loop(t)
  | Tarrow(label, argt, res, _) =>
    let (args, fin) = loop(res);
    ([(labelToString(label), makeFlexible(argt)), ...args], fin)
  | _ => ([], makeFlexible(t))
};
