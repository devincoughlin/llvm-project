//===--- APINotesWriter.h - API Notes Writer ----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the \c APINotesWriter class that writes out source
// API notes data providing additional information about source code as
// a separate input, such as the non-nil/nilable annotations for
// method parameters.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_API_NOTES_WRITER_H
#define LLVM_CLANG_API_NOTES_WRITER_H

#include "clang/APINotes/Types.h"
#include "llvm/Support/VersionTuple.h"

namespace llvm {
  class raw_ostream;
}

namespace clang {

class FileEntry;

namespace api_notes {

/// A class that writes API notes data to a binary representation that can be
/// read by the \c APINotesReader.
class APINotesWriter {
  class Implementation;
  Implementation &Impl;

public:
  /// Create a new API notes writer with the given module name and
  /// (optional) source file.
  APINotesWriter(StringRef moduleName, const FileEntry *sourceFile);
  ~APINotesWriter();

  APINotesWriter(const APINotesWriter &) = delete;
  APINotesWriter &operator=(const APINotesWriter &) = delete;

  /// Write the API notes data to the given stream.
  void writeToStream(llvm::raw_ostream &os);

  /// Add information about a specific Objective-C class or protocol.
  ///
  /// \param name The name of this class/protocol.
  /// \param isClass Whether this is a class (vs. a protocol).
  /// \param info Information about this class/protocol.
  ///
  /// \returns the ID of the class or protocol, which can be used to add
  /// properties and methods to the class/protocol.
  ContextID addObjCContext(StringRef name, bool isClass,
                           const ObjCContextInfo &info,
                           llvm::VersionTuple swiftVersion);

  /// Add information about a specific Objective-C property.
  ///
  /// \param contextID The context in which this property resides.
  /// \param name The name of this property.
  /// \param info Information about this property.
  void addObjCProperty(ContextID contextID, StringRef name,
                       bool isInstanceProperty,
                       const ObjCPropertyInfo &info,
                       llvm::VersionTuple swiftVersion);

  /// Add information about a specific Objective-C method.
  ///
  /// \param contextID The context in which this method resides.
  /// \param selector The selector that names this method.
  /// \param isInstanceMethod Whether this method is an instance method
  /// (vs. a class method).
  /// \param info Information about this method.
  void addObjCMethod(ContextID contextID, ObjCSelectorRef selector,
                     bool isInstanceMethod, const ObjCMethodInfo &info,
                     llvm::VersionTuple swiftVersion);

  /// Add information about a global variable.
  ///
  /// \param name The name of this global variable.
  /// \param info Information about this global variable.
  void addGlobalVariable(StringRef name, const GlobalVariableInfo &info,
                         llvm::VersionTuple swiftVersion);

  /// Add information about a global function.
  ///
  /// \param name The name of this global function.
  /// \param info Information about this global function.
  void addGlobalFunction(StringRef name, const GlobalFunctionInfo &info,
                         llvm::VersionTuple swiftVersion);

  /// Add information about an enumerator.
  ///
  /// \param name The name of this enumerator.
  /// \param info Information about this enumerator.
  void addEnumConstant(StringRef name, const EnumConstantInfo &info,
                       llvm::VersionTuple swiftVersion);

  /// Add information about a tag (struct/union/enum/C++ class).
  ///
  /// \param name The name of this tag.
  /// \param info Information about this tag.
  void addTag(StringRef name, const TagInfo &info,
              llvm::VersionTuple swiftVersion);

  /// Add information about a typedef.
  ///
  /// \param name The name of this typedef.
  /// \param info Information about this typedef.
  void addTypedef(StringRef name, const TypedefInfo &info,
                  llvm::VersionTuple swiftVersion);

  /// Add module options
  void addModuleOptions(ModuleOptions opts);
};

} // end namespace api_notes
} // end namespace clang

#endif // LLVM_CLANG_API_NOTES_WRITER_H

