//===--- TypeAttrs.swift --------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022-2025 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import ASTBridging
import BasicBridging
import SwiftDiagnostics
@_spi(ExperimentalLanguageFeatures) @_spi(RawSyntax) import SwiftSyntax

extension ASTGenVisitor {
  func generateTypeAttributes(_ node: some WithAttributesSyntax) -> [BridgedTypeOrCustomAttr] {
    var attrs: [BridgedTypeOrCustomAttr] = []
    visitIfConfigElements(node.attributes, of: AttributeSyntax.self) { element in
      switch element {
      case .ifConfigDecl(let ifConfigDecl):
        return .ifConfigDecl(ifConfigDecl)
      case .attribute(let attribute):
        return .underlying(attribute)
      }
    } body: { attribute in
      if let attr = self.generateTypeAttribute(attribute: attribute) {
        attrs.append(attr)
      }
    }
    return attrs
  }

  func generateTypeAttribute(attribute node: AttributeSyntax) -> BridgedTypeOrCustomAttr? {
    if let identTy = node.attributeName.as(IdentifierTypeSyntax.self) {
      let attrName = identTy.name.rawText
      let attrKind: swift.TypeAttrKind?
      do {
        let bridgedOptional = BridgedOptionalTypeAttrKind(from: attrName.bridged)
        attrKind = if bridgedOptional.hasValue {
          bridgedOptional.value
        } else {
          nil
        }
      }

      switch attrKind {
      // Simple type attributes.
      case .Autoclosure,
        .Addressable,
        .Concurrent,
        .Escaping,
        .NoEscape,
        .NoDerivative,
        .Async,
        .Sendable,
        .Retroactive,
        .Reparented,
        .Unchecked,
        .Unsafe,
        .Preconcurrency,
        .Local,
        .NoMetadata,
        .Nonisolated,
        .PackGuaranteed,
        .PackInout,
        .PackOut,
        .PackOwned,
        .Pseudogeneric,
        .Yields,
        .YieldMany,
        .YieldOnce,
        .YieldOnce2,
        .Thin,
        .Thick,
        .Unimplementable:
        return self.generateSimpleTypeAttr(attribute: node, kind: attrKind!)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))

      case .Convention:
        return (self.generateConventionTypeAttr(attribute: node)?.asTypeAttribute)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))
      case .Differentiable:
        return (self.generateDifferentiableTypeAttr(attribute: node)?.asTypeAttribute)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))
      case .Lifetime:
        return (self.generateLifetimeTypeAttr(attribute: node)?.asTypeAttribute)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))
      case .OpaqueReturnTypeOf:
        return (self.generateOpaqueReturnTypeOfTypeAttr(attribute: node)?.asTypeAttribute)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))
      case .Isolated:
        return (self.generateIsolatedTypeAttr(attribute: node)?.asTypeAttribute)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))
      case .Called:
        return (self.generateCalledTypeAttr(attribute: node)?.asTypeAttribute)
          .map(BridgedTypeOrCustomAttr.typeAttr(_:))

      // SIL type attributes are not supported.
      case .Autoreleased,
        .BlockStorage,
        .Box,
        .CalleeGuaranteed,
        .CalleeOwned,
        .CapturesGenerics,
        .Direct,
        .DynamicSelf,
        .Error,
        .ErrorIndirect,
        .ErrorUnowned,
        .Guaranteed,
        .GuaranteedAddress,
        .In,
        .InConstant,
        .InGuaranteed,
        .InCXX,
        .Inout,
        .InoutAliasable,
        .MoveOnly,
        .ObjCMetatype,
        .Opened,
        .Out,
        .Owned,
        .PackElement,
        .SILIsolated,
        .SILUnmanaged,
        .SILUnowned,
        .SILWeak,
        .SILSending,
        .SILImplicitLeadingParam,
        .CallerIsolated,
        .UnownedInnerPointer:
        // TODO: Diagnose or fallback to CustomAttr?
        fatalError("SIL type attributes are not supported")
        break;

      case nil:
        // Not a builtin type attribute. Fall back to CustomAttr
        break;
      }
    }

    if let customAttr = self.generateCustomAttr(attribute: node) {
      return .customAttr(customAttr)
    }
    return nil
  }

  func generateSimpleTypeAttr(attribute node: AttributeSyntax, kind: swift.TypeAttrKind) -> BridgedTypeAttribute? {
    // TODO: Diagnose extraneous arguments.
    return BridgedTypeAttribute.createSimple(
      self.ctx,
      kind: kind,
      atLoc: self.generateSourceLoc(node.atSign),
      nameLoc: self.generateSourceLoc(node.attributeName)
    )
  }

  /// E.g.
  ///   ```
  ///   @convention(c)
  ///   @convention(c, cType: "int (*)(int)")
  ///   ```
  func generateConventionTypeAttr(attribute node: AttributeSyntax) -> BridgedConventionTypeAttr? {
    self.generateWithLabeledExprListArguments(attribute: node) { args in
      let nameAndLoc: (name: _, loc: _)? =  self.generateConsumingPlainIdentifierAttrOption(args: &args) {
        (ctx.allocateCopy(string: $0.rawText.bridged), self.generateSourceLoc($0))
      }
      guard let nameAndLoc else {
        return nil
      }

      let typeNameLoc: SourceLoc
      let cTypeName: BridgedStringRef?
      if !args.isEmpty {
        typeNameLoc = self.generateSourceLoc(args.first?.expression)
        cTypeName = self.generateConsumingSimpleStringLiteralAttrOption(args: &args, label: "cType")
        guard cTypeName != nil else {
          return nil
        }
      } else {
        typeNameLoc = nil
        cTypeName = nil
      }

      // `@convention(witness_method: <protocol-name>)` is for SIL only.
      let witnessMethodProtocol: BridgedDeclNameRef = BridgedDeclNameRef()

      return .createParsed(
        self.ctx,
        atLoc: self.generateSourceLoc(node.atSign),
        nameLoc: self.generateSourceLoc(node.attributeName),
        parensRange: self.generateAttrParensRange(attribute: node),
        name: nameAndLoc.name,
        nameLoc: nameAndLoc.loc,
        witnessMethodProtocol: witnessMethodProtocol,
        clangType: cTypeName ?? BridgedStringRef(),
        clangTypeLoc: typeNameLoc
      )
    }
  }

  func generateDifferentiableTypeAttr(attribute node: AttributeSyntax) -> BridgedDifferentiableTypeAttr? {
    // Mirrors `parseDifferentiableTypeAttributeArgument` and its caller in
    // ParseDecl.cpp: an unknown or unsupported kind is an error, while omitting
    // the kind entirely is only a warning and means 'reverse'.
    var differentiability: BridgedDifferentiabilityKind = .normal
    var differentiabilityLoc: SourceLoc = nil

    if let args = node.arguments {
      guard let args = args.as(DifferentiableAttributeArgumentsSyntax.self),
        let kindSpecifier = args.kindSpecifier
      else {
        // Malformed argument clause; the parser has already diagnosed it.
        return nil
      }

      differentiability = self.generateDifferentiabilityKind(text: kindSpecifier.rawText)
      differentiabilityLoc = self.generateSourceLoc(kindSpecifier)
      let kindText = String(syntaxText: kindSpecifier.rawText)
      let replaceWithReverse = CompilerDiagnosticFixIt(
        replace: self.generateCharSourceRange(kindSpecifier),
        with: "reverse"
      )

      switch differentiability {
      case .nonDifferentiable:
        self.diagnose(
          .attr_differentiable_unknown_kind(kindText),
          at: kindSpecifier,
          fixIts: [replaceWithReverse]
        )
        return nil
      case .forward:
        // Only 'reverse' is formally supported today. '_linear' works for
        // testing purposes. '_forward' is rejected.
        self.diagnose(
          .attr_differentiable_kind_not_supported(kindText),
          at: kindSpecifier,
          fixIts: [replaceWithReverse]
        )
        return nil
      default:
        break
      }

      // Anything between the kind and the ')' is extra; the parser records it as
      // unexpected code and has already diagnosed it. The C++ parser likewise
      // builds the attribute and lets the surrounding parse report the leftovers.
    }

    if differentiability == .normal {
      // Bare '@differentiable' means 'reverse', with a deprecation warning.
      let insertReverse = CompilerDiagnosticFixIt(
        replace: self.generateCharSourceRange(
          start: node.attributeName.endPositionBeforeTrailingTrivia,
          length: SourceLength(utf8Length: 0)
        ),
        with: "(reverse)"
      )
      self.diagnose(
        .attr_differentiable_expected_reverse,
        at: node.attributeName,
        fixIts: [insertReverse]
      )
      differentiability = .reverse
    }

    return .createParsed(
      self.ctx,
      atLoc: self.generateSourceLoc(node.atSign),
      nameLoc: self.generateSourceLoc(node.attributeName),
      parensRange: self.generateAttrParensRange(attribute: node),
      kind: differentiability,
      kindLoc: differentiabilityLoc
    )
  }

  func generateLifetimeTypeAttr(attribute node: AttributeSyntax) -> BridgedLifetimeTypeAttr? {
    guard let entry = self.generateLifetimeEntry(attribute: node) else {
      return nil
    }

    return .createParsed(
      self.ctx,
      atLoc: self.generateSourceLoc(node.atSign),
      nameLoc: self.generateSourceLoc(node.attributeName),
      parensRange: self.generateAttrParensRange(attribute: node),
      entry: entry
    )
  }
  
  func generateIsolatedTypeAttr(attribute node: AttributeSyntax) -> BridgedIsolatedTypeAttr? {
    let isolationKindLoc = self.generateSourceLoc(node.arguments)
    let isolationKind: BridgedIsolatedTypeAttrIsolationKind? = self.generateSingleAttrOption(
      attribute: node,
      {
        switch $0.rawText {
        case "any": return .dynamicIsolation
        default:
          // TODO: Diagnose.
          return nil
        }
      }
    )
    guard let isolationKind else {
      return nil
    }

    return .createParsed(
      self.ctx,
      atLoc: self.generateSourceLoc(node.atSign),
      nameLoc: self.generateSourceLoc(node.attributeName),
      parensRange: self.generateAttrParensRange(attribute: node),
      isolationKind: isolationKind,
      isolationKindLoc: isolationKindLoc
    )
  }

  func generateCalledTypeAttr(attribute node: AttributeSyntax) -> BridgedCalledTypeAttr? {
    let semanticsLoc = self.generateSourceLoc(node.arguments)
    let semantics: BridgedCalledTypeAttrSemantics? = self.generateSingleAttrOption(
      attribute: node,
      {
        switch $0.rawText {
        case "once": return .once
        default:
          // TODO: Diagnose.
          return nil
        }
      }
    )
    guard let semantics else {
      return nil
    }

    return .createParsed(
      self.ctx,
      atLoc: self.generateSourceLoc(node.atSign),
      nameLoc: self.generateSourceLoc(node.attributeName),
      parensRange: self.generateAttrParensRange(attribute: node),
      semantics: semantics,
      semanticsLoc: semanticsLoc
    )
  }

  /// E.g.
  ///   ```
  ///   @_opaqueReturnTypeOf("$sMangledName", 4)
  ///   ```
  func generateOpaqueReturnTypeOfTypeAttr(attribute node: AttributeSyntax) -> BridgedOpaqueReturnTypeOfTypeAttr? {
    self.generateWithLabeledExprListArguments(attribute: node) { args in
      let mangledLoc = self.generateSourceLoc(args.first?.expression)
      let mangledName = self.generateConsumingSimpleStringLiteralAttrOption(args: &args)
      guard let mangledName else {
        return nil
      }

      let indexLoc = self.generateSourceLoc(args.first?.expression)
      let index: Int? = self.generateConsumingAttrOption(args: &args, label: nil) { expr in
        guard let intExpr = expr.as(IntegerLiteralExprSyntax.self) else {
          // TODO: Diagnostics.
          fatalError("expected integer literal")
          // return nil
        }
        return intExpr.representedLiteralValue
      }
      guard let index else {
        return nil
      }

      return .createParsed(
        self.ctx,
        atLoc: self.generateSourceLoc(node.atSign),
        nameLoc: self.generateSourceLoc(node.attributeName),
        parensRange: self.generateAttrParensRange(attribute: node),
        mangled: mangledName,
        mangledLoc: mangledLoc,
        index: index,
        indexLoc: indexLoc
      )
    }

  }
  
  func generateAttrParensRange(attribute node: AttributeSyntax) -> SourceRange {
    guard let lParen = node.leftParen else {
      return .init()
    }
    return self.generateSourceRange(start: lParen, end: node.lastToken(viewMode: .sourceAccurate)!)
  }
}
