open RResult;
open TopTypes;
open Infix;
module J = JsonShort;

let maybeHash = (h, k) =>
  if (Hashtbl.mem(h, k)) {
    Some(Hashtbl.find(h, k));
  } else {
    None;
  };

let getPackage =
  Packages.getPackage(
    ~reportDiagnostics=NotificationHandlers.reportDiagnostics,
  );

let handlers:
  list((string, (state, Json.t) => result((state, Json.t), string))) = [
  (
    "textDocument/definition",
    (state, params) => {
      let%try (uri, pos) = Protocol.rPositionParams(params);
      let%try package = getPackage(uri, state);
      let%try (file, extra) = State.fileForUri(state, ~package, uri);

      let position = Utils.cmtLocFromVscode(pos);

      {
        let%opt (uri, loc) =
          References.definitionForPos(
            ~pathsForModule=package.pathsForModule,
            ~file,
            ~extra,
            ~getUri=State.fileForUri(state, ~package),
            ~getModule=State.fileForModule(state, ~package),
            position,
          );
        Some(
          Ok((
            state,
            Json.Object([
              ("uri", Json.String(uri)),
              ("range", Protocol.rangeOfLoc(loc)),
            ]),
          )),
        );
      }
      |? Ok((state, Json.Null));
    },
  ),
  (
    "textDocument/completion",
    (state, params) => {
      let%try (uri, pos) = Protocol.rPositionParams(params);
      let maybeText =
        switch (Hashtbl.find_opt(state.documentText, uri)) {
        | Some((text, _version, _isClean)) => Some(text)
        | None => None
        };
      let%try package = getPackage(uri, state);
      let%try full = State.getBestDefinitions(uri, state, ~package);
      let completions =
        NewCompletions.computeCompletions(
          ~full,
          ~maybeText,
          ~package,
          ~pos,
          ~state,
        );
      Ok((state, completions));
    },
  ),
  (
    "textDocument/documentHighlight",
    (state, params) => {
      let%try (uri, pos) = Protocol.rPositionParams(params);
      let%try package = getPackage(uri, state);

      let pos = Utils.cmtLocFromVscode(pos);
      let refs =
        switch (State.fileForUri(state, ~package, uri) |> toOptionAndLog) {
        | None => None
        | Some((file, extra)) => References.refsForPos(~file, ~extra, pos)
        };
      Ok((
        state,
        switch (refs) {
        | None => J.null
        | Some(refs) =>
          J.l(
            refs
            |> List.map(loc =>
                 J.o([
                   ("range", Protocol.rangeOfLoc(loc)),
                   ("kind", J.i(2)),
                 ])
               ),
          )
        },
      ));
    },
  ),
  (
    "textDocument/references",
    (state, params) => {
      let%try (uri, pos) = Protocol.rPositionParams(params);
      let%try package = getPackage(uri, state);
      let%try_wrap (file, extra) = State.fileForUri(state, ~package, uri);
      Infix.(
        {
          let%opt (_, loc) =
            References.locForPos(~extra, Utils.cmtLocFromVscode(pos));
          let%opt allReferences =
            References.allReferencesForLoc(
              ~pathsForModule=package.pathsForModule,
              ~file,
              ~extra,
              ~allModules=package.localModules,
              ~getUri=State.fileForUri(state, ~package),
              ~getModule=State.fileForModule(state, ~package),
              ~getExtra=State.extraForModule(state, ~package),
              loc,
            )
            |> toOptionAndLog;
          Some((
            state,
            J.l(
              allReferences
              |> List.map(((fname, references)) => {
                   let locs =
                     fname == uri
                       ? List.filter(
                           loc => !Protocol.locationContains(loc, pos),
                           references,
                         )
                       : references;
                   locs
                   |> List.map(loc => Protocol.locationOfLoc(~fname, loc));
                 })
              |> List.concat,
            ),
          ));
        }
        |? (state, J.null)
      );
    },
  ),
  (
    "textDocument/rename",
    (state, params) => {
      let%try (uri, pos) = Protocol.rPositionParams(params);
      let%try package = getPackage(uri, state);
      let%try (file, extra) = State.fileForUri(state, ~package, uri);
      let%try newName = RJson.get("newName", params);
      Infix.(
        {
          let%opt (_, loc) =
            References.locForPos(~extra, Utils.cmtLocFromVscode(pos));
          let%opt allReferences =
            References.allReferencesForLoc(
              ~file,
              ~extra,
              ~pathsForModule=package.pathsForModule,
              ~allModules=package.localModules,
              ~getModule=State.fileForModule(state, ~package),
              ~getUri=State.fileForUri(state, ~package),
              ~getExtra=State.extraForModule(state, ~package),
              loc,
            )
            |> toOptionAndLog;
          Some(
            Ok((
              state,
              J.o([
                (
                  "changes",
                  J.o(
                    allReferences
                    |> List.map(((fname, references)) =>
                         (
                           fname,
                           J.l(
                             references
                             |> List.map(loc =>
                                  J.o([
                                    ("range", Protocol.rangeOfLoc(loc)),
                                    ("newText", newName),
                                  ])
                                ),
                           ),
                         )
                       ),
                  ),
                ),
              ]),
            )),
          );
        }
        |? Ok((state, J.null))
      );
    },
  ),
  (
    "textDocument/codeLens",
    (state, params) => {
      open InfixResult;
      let%try uri =
        params
        |> RJson.get("textDocument")
        |?> RJson.get("uri")
        |?> RJson.string;
      /* let%try package = getPackage(uri, state); */
      switch (getPackage(uri, state)) {
      | Error(message) =>
        let items = [
          (
            "Unable to load compilation data: " ++ message,
            {
              Location.loc_start: {
                Lexing.pos_fname: "",
                pos_lnum: 1,
                pos_bol: 0,
                pos_cnum: 0,
              },
              Location.loc_end: {
                Lexing.pos_fname: "",
                pos_lnum: 1,
                pos_bol: 0,
                pos_cnum: 0,
              },
              loc_ghost: false,
            },
          ),
        ];
        Ok((
          state,
          J.l(
            items
            |> List.map(((text, loc)) =>
                 J.o([
                   ("range", Protocol.rangeOfLoc(loc)),
                   (
                     "command",
                     J.o([("title", J.s(text)), ("command", J.s(""))]),
                   ),
                 ])
               ),
          ),
        ));
      | Ok(package) =>
        /* Log.log("<< codleens me please"); */

        let topLoc = {
          Location.loc_start: {
            Lexing.pos_fname: "",
            pos_lnum: 1,
            pos_bol: 0,
            pos_cnum: 0,
          },
          Location.loc_end: {
            Lexing.pos_fname: "",
            pos_lnum: 1,
            pos_bol: 0,
            pos_cnum: 0,
          },
          loc_ghost: false,
        };

        let getLensItems = ({SharedTypes.file, extra}) => {
          /* getTypeLensTopLevel gives the list of per-value type codeLens
             for every value in a module topLevel */
          let rec getTypeLensTopLevel = topLevel => {
            switch (topLevel) {
            | [] => []
            | [{SharedTypes.name: {loc}, item}, ...tlp] =>
              let currentCl =
                switch (item) {
                | SharedTypes.MValue(typ) => [
                    (typ |> Shared.typeToString, loc),
                  ]
                | Module(Structure({topLevel})) =>
                  getTypeLensTopLevel(topLevel)
                | _ => []
                };
              List.append(currentCl, getTypeLensTopLevel(tlp));
            };
          };
          let showToplevelTypes = state.settings.perValueCodelens; /* TODO config option */
          let lenses =
            showToplevelTypes
              ? file.contents.topLevel |> getTypeLensTopLevel : [];
          let showOpens = state.settings.opensCodelens;
          let lenses =
            showOpens
              ? lenses
                @ {
                  CodeLens.forOpens(extra);
                }
              : lenses;

          let depsList =
            List.map(fst, SharedTypes.hashList(extra.externalReferences));
          let depsString =
            depsList == [] ? "[none]" : String.concat(", ", depsList);
          let lenses =
            state.settings.dependenciesCodelens == true
              ? [("Dependencies: " ++ depsString, topLoc), ...lenses]
              : lenses;

          lenses;
        };

        let items = {
          let full =
            switch (State.getCompilationResult(uri, state, ~package)) {
            | Ok(Success(_, full)) => Ok(Some(full))
            | Ok(_) => Ok(State.getLastDefinitions(uri, state))
            | Error(m) => Error(m)
            };
          switch (full) {
          | Error(message) => [(message, topLoc)]
          | Ok(None) => [("Unable to get compilation data", topLoc)]
          | Ok(Some(full)) => getLensItems(full)
          };
        };
        Ok((
          state,
          J.l(
            items
            |> List.map(((text, loc)) =>
                 J.o([
                   ("range", Protocol.rangeOfLoc(loc)),
                   (
                     "command",
                     J.o([("title", J.s(text)), ("command", J.s(""))]),
                   ),
                 ])
               ),
          ),
        ));
      };
    },
  ),
  (
    "textDocument/hover",
    (state, params) => {
      let%try (uri, pos) = Protocol.rPositionParams(params);
      let%try package = getPackage(uri, state);
      let%try (file, extra) = State.fileForUri(state, ~package, uri);

      {
        let pos = Utils.cmtLocFromVscode(pos);
        let%opt (location, loc) = References.locForPos(~extra, pos);
        let%opt text =
          Hover.newHover(
            ~rootUri=state.rootUri,
            ~file,
            ~getModule=State.fileForModule(state, ~package),
            ~markdown=!state.settings.clientNeedsPlainText,
            ~showPath=state.settings.showModulePathOnHover,
            loc,
          );
        Some(
          Ok((
            state,
            J.o([
              ("range", Protocol.rangeOfLoc(location)),
              (
                "contents",
                text
                |> Protocol.contentKind(!state.settings.clientNeedsPlainText),
              ),
            ]),
          )),
        );
      }
      |? Ok((state, J.null));
    },
  ),
  (
    "textDocument/documentSymbol",
    (state, params) => {
      open InfixResult;
      let%try uri =
        params
        |> RJson.get("textDocument")
        |?> RJson.get("uri")
        |?> RJson.string;
      let%try package = getPackage(uri, state);

      let%try (file, _extra) = State.fileForUri(state, ~package, uri);
      open SharedTypes;

      let rec getItems = ({topLevel}) => {
        let fn = ({name: {txt}, extentLoc, item}) => {
          let (item, siblings) =
            switch (item) {
            | MValue(v) => (v |> Shared.variableKind, [])
            | MType(t, _) => (t.decl |> Shared.declarationKind, [])
            | Module(Structure(contents)) => (Module, getItems(contents))
            | Module(Ident(_)) => (Module, [])
            };
          if (extentLoc.loc_ghost) {
            siblings;
          } else {
            [(txt, extentLoc, item), ...siblings];
          };
        };
        let x = topLevel |> List.map(fn) |> List.concat;
        x;
      };

      getItems(file.contents)
      |> (
        items => {
          Ok((
            state,
            J.l(
              items
              |> List.map(((name, loc, typ)) =>
                   J.o([
                     ("name", J.s(name)),
                     ("kind", J.i(Protocol.symbolKind(typ))),
                     ("location", Protocol.locationOfLoc(loc)),
                     /* ("containerName", s(String.concat(".", path))) */
                   ])
                 ),
            ),
          ));
        }
      );
    },
  ),
];
