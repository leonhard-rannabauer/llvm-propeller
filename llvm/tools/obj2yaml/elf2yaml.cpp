//===------ utils/elf2yaml.cpp - obj2yaml conversion tool -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "obj2yaml.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/ObjectYAML/DWARFYAML.h"
#include "llvm/ObjectYAML/ELFYAML.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/YAMLTraits.h"

using namespace llvm;

namespace {

template <class ELFT>
class ELFDumper {
  typedef object::Elf_Sym_Impl<ELFT> Elf_Sym;
  typedef typename ELFT::Dyn Elf_Dyn;
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Word Elf_Word;
  typedef typename ELFT::Rel Elf_Rel;
  typedef typename ELFT::Rela Elf_Rela;
  using Elf_Relr = typename ELFT::Relr;
  using Elf_Nhdr = typename ELFT::Nhdr;
  using Elf_Note = typename ELFT::Note;

  ArrayRef<Elf_Shdr> Sections;
  ArrayRef<Elf_Sym> SymTable;

  DenseMap<StringRef, uint32_t> UsedSectionNames;
  std::vector<std::string> SectionNames;

  DenseMap<StringRef, uint32_t> UsedSymbolNames;
  std::vector<std::string> SymbolNames;

  BumpPtrAllocator StringAllocator;

  Expected<StringRef> getUniquedSectionName(const Elf_Shdr *Sec);
  Expected<StringRef> getUniquedSymbolName(const Elf_Sym *Sym,
                                           StringRef StrTable,
                                           const Elf_Shdr *SymTab);
  Expected<StringRef> getSymbolName(uint32_t SymtabNdx, uint32_t SymbolNdx);

  const object::ELFFile<ELFT> &Obj;
  std::unique_ptr<DWARFContext> DWARFCtx;
  ArrayRef<Elf_Word> ShndxTable;

  Expected<std::vector<ELFYAML::ProgramHeader>>
  dumpProgramHeaders(ArrayRef<std::unique_ptr<ELFYAML::Chunk>> Sections);

  Optional<DWARFYAML::Data>
  dumpDWARFSections(std::vector<std::unique_ptr<ELFYAML::Chunk>> &Sections);

  Error dumpSymbols(const Elf_Shdr *Symtab,
                    std::vector<ELFYAML::Symbol> &Symbols);
  Error dumpSymbol(const Elf_Sym *Sym, const Elf_Shdr *SymTab,
                   StringRef StrTable, ELFYAML::Symbol &S);
  Expected<std::vector<std::unique_ptr<ELFYAML::Chunk>>> dumpSections();
  Error dumpCommonSection(const Elf_Shdr *Shdr, ELFYAML::Section &S);
  Error dumpCommonRelocationSection(const Elf_Shdr *Shdr,
                                    ELFYAML::RelocationSection &S);
  template <class RelT>
  Error dumpRelocation(const RelT *Rel, const Elf_Shdr *SymTab,
                       ELFYAML::Relocation &R);

  Expected<ELFYAML::AddrsigSection *> dumpAddrsigSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::LinkerOptionsSection *>
  dumpLinkerOptionsSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::DependentLibrariesSection *>
  dumpDependentLibrariesSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::CallGraphProfileSection *>
  dumpCallGraphProfileSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::DynamicSection *> dumpDynamicSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::RelocationSection *> dumpRelocSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::RelrSection *> dumpRelrSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::RawContentSection *>
  dumpContentSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::SymtabShndxSection *>
  dumpSymtabShndxSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::NoBitsSection *> dumpNoBitsSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::HashSection *> dumpHashSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::NoteSection *> dumpNoteSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::GnuHashSection *> dumpGnuHashSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::VerdefSection *> dumpVerdefSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::SymverSection *> dumpSymverSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::VerneedSection *> dumpVerneedSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::Group *> dumpGroup(const Elf_Shdr *Shdr);
  Expected<ELFYAML::MipsABIFlags *> dumpMipsABIFlags(const Elf_Shdr *Shdr);
  Expected<ELFYAML::StackSizesSection *>
  dumpStackSizesSection(const Elf_Shdr *Shdr);
  Expected<ELFYAML::RawContentSection *>
  dumpPlaceholderSection(const Elf_Shdr *Shdr);

  bool shouldPrintSection(const ELFYAML::Section &S, const Elf_Shdr &SHdr,
                          Optional<DWARFYAML::Data> DWARF);

public:
  ELFDumper(const object::ELFFile<ELFT> &O, std::unique_ptr<DWARFContext> DCtx);
  Expected<ELFYAML::Object *> dump();
};

}

template <class ELFT>
ELFDumper<ELFT>::ELFDumper(const object::ELFFile<ELFT> &O,
                           std::unique_ptr<DWARFContext> DCtx)
    : Obj(O), DWARFCtx(std::move(DCtx)) {}

template <class ELFT>
Expected<StringRef>
ELFDumper<ELFT>::getUniquedSectionName(const Elf_Shdr *Sec) {
  unsigned SecIndex = Sec - &Sections[0];
  assert(&Sections[SecIndex] == Sec);
  if (!SectionNames[SecIndex].empty())
    return SectionNames[SecIndex];

  auto NameOrErr = Obj.getSectionName(*Sec);
  if (!NameOrErr)
    return NameOrErr;
  StringRef Name = *NameOrErr;
  // In some specific cases we might have more than one section without a
  // name (sh_name == 0). It normally doesn't happen, but when we have this case
  // it doesn't make sense to uniquify their names and add noise to the output.
  if (Name.empty())
    return "";

  std::string &Ret = SectionNames[SecIndex];

  auto It = UsedSectionNames.insert({Name, 0});
  if (!It.second)
    Ret = ELFYAML::appendUniqueSuffix(Name, Twine(++It.first->second));
  else
    Ret = std::string(Name);
  return Ret;
}

template <class ELFT>
Expected<StringRef>
ELFDumper<ELFT>::getUniquedSymbolName(const Elf_Sym *Sym, StringRef StrTable,
                                      const Elf_Shdr *SymTab) {
  Expected<StringRef> SymbolNameOrErr = Sym->getName(StrTable);
  if (!SymbolNameOrErr)
    return SymbolNameOrErr;
  StringRef Name = *SymbolNameOrErr;
  if (Name.empty() && Sym->getType() == ELF::STT_SECTION) {
    auto ShdrOrErr = Obj.getSection(*Sym, SymTab, ShndxTable);
    if (!ShdrOrErr)
      return ShdrOrErr.takeError();
    return getUniquedSectionName(*ShdrOrErr);
  }

  // Symbols in .symtab can have duplicate names. For example, it is a common
  // situation for local symbols in a relocatable object. Here we assign unique
  // suffixes for such symbols so that we can differentiate them.
  if (SymTab->sh_type == ELF::SHT_SYMTAB) {
    unsigned Index = Sym - SymTable.data();
    if (!SymbolNames[Index].empty())
      return SymbolNames[Index];

    auto It = UsedSymbolNames.insert({Name, 0});
    if (!It.second)
      SymbolNames[Index] =
          ELFYAML::appendUniqueSuffix(Name, Twine(++It.first->second));
    else
      SymbolNames[Index] = std::string(Name);
    return SymbolNames[Index];
  }

  return Name;
}

template <class ELFT>
bool ELFDumper<ELFT>::shouldPrintSection(const ELFYAML::Section &S,
                                         const Elf_Shdr &SHdr,
                                         Optional<DWARFYAML::Data> DWARF) {
  // We only print the SHT_NULL section at index 0 when it
  // has at least one non-null field, because yaml2obj
  // normally creates the zero section at index 0 implicitly.
  if (S.Type == ELF::SHT_NULL && (&SHdr == &Sections[0])) {
    const uint8_t *Begin = reinterpret_cast<const uint8_t *>(&SHdr);
    const uint8_t *End = Begin + sizeof(Elf_Shdr);
    return std::find_if(Begin, End, [](uint8_t V) { return V != 0; }) != End;
  }

  // Normally we use "DWARF:" to describe contents of DWARF sections. Sometimes
  // the content of DWARF sections can be successfully parsed into the "DWARF:"
  // entry but their section headers may have special flags, entry size, address
  // alignment, etc. We will preserve the header for them under such
  // circumstances.
  StringRef SecName = S.Name.substr(1);
  if (DWARF && DWARF->getNonEmptySectionNames().count(SecName)) {
    if (const ELFYAML::RawContentSection *RawSec =
            dyn_cast<const ELFYAML::RawContentSection>(&S)) {
      if (RawSec->Type != ELF::SHT_PROGBITS || !RawSec->Link.empty() ||
          RawSec->Info || RawSec->AddressAlign != 1 || RawSec->Address ||
          RawSec->EntSize)
        return true;

      ELFYAML::ELF_SHF ShFlags = RawSec->Flags.getValueOr(ELFYAML::ELF_SHF(0));

      if (SecName == "debug_str")
        return ShFlags != ELFYAML::ELF_SHF(ELF::SHF_MERGE | ELF::SHF_STRINGS);

      return ShFlags != 0;
    }
  }

  // Normally we use "Symbols:" and "DynamicSymbols:" to describe contents of
  // symbol tables. We also build and emit corresponding string tables
  // implicitly. But sometimes it is important to preserve positions and virtual
  // addresses of allocatable sections, e.g. for creating program headers.
  // Generally we are trying to reduce noise in the YAML output. Because
  // of that we do not print non-allocatable versions of such sections and
  // assume they are placed at the end.
  if (S.Type == ELF::SHT_STRTAB || S.Type == ELF::SHT_SYMTAB ||
      S.Type == ELF::SHT_DYNSYM)
    return S.Flags.getValueOr(ELFYAML::ELF_SHF(0)) & ELF::SHF_ALLOC;

  return true;
}

template <class ELFT> Expected<ELFYAML::Object *> ELFDumper<ELFT>::dump() {
  auto Y = std::make_unique<ELFYAML::Object>();

  // Dump header. We do not dump EPh* and ESh* fields. When not explicitly set,
  // the values are set by yaml2obj automatically and there is no need to dump
  // them here.
  Y->Header.Class = ELFYAML::ELF_ELFCLASS(Obj.getHeader().getFileClass());
  Y->Header.Data = ELFYAML::ELF_ELFDATA(Obj.getHeader().getDataEncoding());
  Y->Header.OSABI = Obj.getHeader().e_ident[ELF::EI_OSABI];
  Y->Header.ABIVersion = Obj.getHeader().e_ident[ELF::EI_ABIVERSION];
  Y->Header.Type = Obj.getHeader().e_type;
  Y->Header.Machine = ELFYAML::ELF_EM(Obj.getHeader().e_machine);
  Y->Header.Flags = Obj.getHeader().e_flags;
  Y->Header.Entry = Obj.getHeader().e_entry;

  // Dump sections
  auto SectionsOrErr = Obj.sections();
  if (!SectionsOrErr)
    return SectionsOrErr.takeError();
  Sections = *SectionsOrErr;
  SectionNames.resize(Sections.size());

  // Dump symbols. We need to do this early because other sections might want
  // to access the deduplicated symbol names that we also create here.
  const Elf_Shdr *SymTab = nullptr;
  const Elf_Shdr *SymTabShndx = nullptr;
  const Elf_Shdr *DynSymTab = nullptr;

  for (const Elf_Shdr &Sec : Sections) {
    if (Sec.sh_type == ELF::SHT_SYMTAB) {
      SymTab = &Sec;
    } else if (Sec.sh_type == ELF::SHT_DYNSYM) {
      DynSymTab = &Sec;
    } else if (Sec.sh_type == ELF::SHT_SYMTAB_SHNDX) {
      // ABI allows us to have one SHT_SYMTAB_SHNDX for each symbol table.
      // We only support having the SHT_SYMTAB_SHNDX for SHT_SYMTAB now.
      if (SymTabShndx)
        return createStringError(
            errc::not_supported,
            "multiple SHT_SYMTAB_SHNDX sections are not supported");
      SymTabShndx = &Sec;
    }
  }

  // We need to locate the SHT_SYMTAB_SHNDX section early, because it might be
  // needed for dumping symbols.
  if (SymTabShndx) {
    if (!SymTab ||
        SymTabShndx->sh_link != (unsigned)(SymTab - Sections.begin()))
      return createStringError(
          errc::not_supported,
          "only SHT_SYMTAB_SHNDX associated with SHT_SYMTAB are supported");

    auto TableOrErr = Obj.getSHNDXTable(*SymTabShndx);
    if (!TableOrErr)
      return TableOrErr.takeError();
    ShndxTable = *TableOrErr;
  }

  if (SymTab) {
    Y->Symbols.emplace();
    if (Error E = dumpSymbols(SymTab, *Y->Symbols))
      return std::move(E);
  }

  if (DynSymTab) {
    Y->DynamicSymbols.emplace();
    if (Error E = dumpSymbols(DynSymTab, *Y->DynamicSymbols))
      return std::move(E);
  }

  // We dump all sections first. It is simple and allows us to verify that all
  // sections are valid and also to generalize the code. But we are not going to
  // keep all of them in the final output (see comments for
  // 'shouldPrintSection()'). Undesired chunks will be removed later.
  Expected<std::vector<std::unique_ptr<ELFYAML::Chunk>>> ChunksOrErr =
      dumpSections();
  if (!ChunksOrErr)
    return ChunksOrErr.takeError();
  std::vector<std::unique_ptr<ELFYAML::Chunk>> Chunks = std::move(*ChunksOrErr);

  // Dump program headers.
  Expected<std::vector<ELFYAML::ProgramHeader>> PhdrsOrErr =
      dumpProgramHeaders(Chunks);
  if (!PhdrsOrErr)
    return PhdrsOrErr.takeError();
  Y->ProgramHeaders = std::move(*PhdrsOrErr);

  // Dump DWARF sections.
  Y->DWARF = dumpDWARFSections(Chunks);

  llvm::erase_if(Chunks, [this, &Y](const std::unique_ptr<ELFYAML::Chunk> &C) {
    const ELFYAML::Section &S = cast<ELFYAML::Section>(*C.get());
    return !shouldPrintSection(S, Sections[S.OriginalSecNdx], Y->DWARF);
  });

  Y->Chunks = std::move(Chunks);
  return Y.release();
}

template <class ELFT>
static bool isInSegment(const ELFYAML::Section &Sec,
                        const typename ELFT::Shdr &SHdr,
                        const typename ELFT::Phdr &Phdr) {
  if (Sec.Type == ELF::SHT_NULL)
    return false;

  // A section is within a segment when its location in a file is within the
  // [p_offset, p_offset + p_filesz] region.
  bool FileOffsetsMatch =
      SHdr.sh_offset >= Phdr.p_offset &&
      (SHdr.sh_offset + SHdr.sh_size <= Phdr.p_offset + Phdr.p_filesz);

  bool VirtualAddressesMatch = SHdr.sh_addr >= Phdr.p_vaddr &&
                               SHdr.sh_addr <= Phdr.p_vaddr + Phdr.p_memsz;

  if (FileOffsetsMatch) {
    // An empty section on the edges of a program header can be outside of the
    // virtual address space of the segment. This means it is not included in
    // the segment and we should ignore it.
    if (SHdr.sh_size == 0 && (SHdr.sh_offset == Phdr.p_offset ||
                              SHdr.sh_offset == Phdr.p_offset + Phdr.p_filesz))
      return VirtualAddressesMatch;
    return true;
  }

  // SHT_NOBITS sections usually occupy no physical space in a file. Such
  // sections belong to a segment when they reside in the segment's virtual
  // address space.
  if (Sec.Type != ELF::SHT_NOBITS)
    return false;
  return VirtualAddressesMatch;
}

template <class ELFT>
Expected<std::vector<ELFYAML::ProgramHeader>>
ELFDumper<ELFT>::dumpProgramHeaders(
    ArrayRef<std::unique_ptr<ELFYAML::Chunk>> Chunks) {
  std::vector<ELFYAML::ProgramHeader> Ret;
  Expected<typename ELFT::PhdrRange> PhdrsOrErr = Obj.program_headers();
  if (!PhdrsOrErr)
    return PhdrsOrErr.takeError();

  for (const typename ELFT::Phdr &Phdr : *PhdrsOrErr) {
    ELFYAML::ProgramHeader PH;
    PH.Type = Phdr.p_type;
    PH.Flags = Phdr.p_flags;
    PH.VAddr = Phdr.p_vaddr;
    PH.PAddr = Phdr.p_paddr;

    // yaml2obj sets the alignment of a segment to 1 by default.
    // We do not print the default alignment to reduce noise in the output.
    if (Phdr.p_align != 1)
      PH.Align = static_cast<llvm::yaml::Hex64>(Phdr.p_align);

    // Here we match sections with segments.
    // It is not possible to have a non-Section chunk, because
    // obj2yaml does not create Fill chunks.
    for (const std::unique_ptr<ELFYAML::Chunk> &C : Chunks) {
      ELFYAML::Section &S = cast<ELFYAML::Section>(*C.get());
      if (isInSegment<ELFT>(S, Sections[S.OriginalSecNdx], Phdr))
        PH.Sections.push_back({S.Name});
    }

    Ret.push_back(PH);
  }

  return Ret;
}

template <class ELFT>
Optional<DWARFYAML::Data> ELFDumper<ELFT>::dumpDWARFSections(
    std::vector<std::unique_ptr<ELFYAML::Chunk>> &Sections) {
  DWARFYAML::Data DWARF;
  for (std::unique_ptr<ELFYAML::Chunk> &C : Sections) {
    if (!C->Name.startswith(".debug_"))
      continue;

    if (ELFYAML::RawContentSection *RawSec =
            dyn_cast<ELFYAML::RawContentSection>(C.get())) {
      Error Err = Error::success();
      cantFail(std::move(Err));

      if (RawSec->Name == ".debug_aranges")
        Err = dumpDebugARanges(*DWARFCtx.get(), DWARF);
      else if (RawSec->Name == ".debug_str")
        Err = dumpDebugStrings(*DWARFCtx.get(), DWARF);
      else if (RawSec->Name == ".debug_ranges")
        Err = dumpDebugRanges(*DWARFCtx.get(), DWARF);
      else if (RawSec->Name == ".debug_addr")
        Err = dumpDebugAddr(*DWARFCtx.get(), DWARF);
      else
        continue;

      // If the DWARF section cannot be successfully parsed, emit raw content
      // instead of an entry in the DWARF section of the YAML.
      if (Err)
        consumeError(std::move(Err));
      else
        RawSec->Content.reset();
    }
  }

  if (DWARF.getNonEmptySectionNames().empty())
    return None;
  return DWARF;
}

template <class ELFT>
Expected<ELFYAML::RawContentSection *>
ELFDumper<ELFT>::dumpPlaceholderSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::RawContentSection>();
  if (Error E = dumpCommonSection(Shdr, *S.get()))
    return std::move(E);
  return S.release();
}

template <class ELFT>
Expected<std::vector<std::unique_ptr<ELFYAML::Chunk>>>
ELFDumper<ELFT>::dumpSections() {
  std::vector<std::unique_ptr<ELFYAML::Chunk>> Ret;
  auto Add = [&](Expected<ELFYAML::Chunk *> SecOrErr) -> Error {
    if (!SecOrErr)
      return SecOrErr.takeError();
    Ret.emplace_back(*SecOrErr);
    return Error::success();
  };

  auto GetDumper = [this](unsigned Type)
      -> std::function<Expected<ELFYAML::Chunk *>(const Elf_Shdr *)> {
    switch (Type) {
    case ELF::SHT_DYNAMIC:
      return [this](const Elf_Shdr *S) { return dumpDynamicSection(S); };
    case ELF::SHT_SYMTAB_SHNDX:
      return [this](const Elf_Shdr *S) { return dumpSymtabShndxSection(S); };
    case ELF::SHT_REL:
    case ELF::SHT_RELA:
      return [this](const Elf_Shdr *S) { return dumpRelocSection(S); };
    case ELF::SHT_RELR:
      return [this](const Elf_Shdr *S) { return dumpRelrSection(S); };
    case ELF::SHT_GROUP:
      return [this](const Elf_Shdr *S) { return dumpGroup(S); };
    case ELF::SHT_MIPS_ABIFLAGS:
      return [this](const Elf_Shdr *S) { return dumpMipsABIFlags(S); };
    case ELF::SHT_NOBITS:
      return [this](const Elf_Shdr *S) { return dumpNoBitsSection(S); };
    case ELF::SHT_NOTE:
      return [this](const Elf_Shdr *S) { return dumpNoteSection(S); };
    case ELF::SHT_HASH:
      return [this](const Elf_Shdr *S) { return dumpHashSection(S); };
    case ELF::SHT_GNU_HASH:
      return [this](const Elf_Shdr *S) { return dumpGnuHashSection(S); };
    case ELF::SHT_GNU_verdef:
      return [this](const Elf_Shdr *S) { return dumpVerdefSection(S); };
    case ELF::SHT_GNU_versym:
      return [this](const Elf_Shdr *S) { return dumpSymverSection(S); };
    case ELF::SHT_GNU_verneed:
      return [this](const Elf_Shdr *S) { return dumpVerneedSection(S); };
    case ELF::SHT_LLVM_ADDRSIG:
      return [this](const Elf_Shdr *S) { return dumpAddrsigSection(S); };
    case ELF::SHT_LLVM_LINKER_OPTIONS:
      return [this](const Elf_Shdr *S) { return dumpLinkerOptionsSection(S); };
    case ELF::SHT_LLVM_DEPENDENT_LIBRARIES:
      return [this](const Elf_Shdr *S) {
        return dumpDependentLibrariesSection(S);
      };
    case ELF::SHT_LLVM_CALL_GRAPH_PROFILE:
      return
          [this](const Elf_Shdr *S) { return dumpCallGraphProfileSection(S); };
    case ELF::SHT_STRTAB:
    case ELF::SHT_SYMTAB:
    case ELF::SHT_DYNSYM:
      // The contents of these sections are described by other parts of the YAML
      // file. But we still want to dump them, because their properties can be
      // important. See comments for 'shouldPrintSection()' for more details.
      return [this](const Elf_Shdr *S) { return dumpPlaceholderSection(S); };
    default:
      return nullptr;
    }
  };

  for (const Elf_Shdr &Sec : Sections) {
    // We have dedicated dumping functions for most of the section types.
    // Try to use one of them first.
    if (std::function<Expected<ELFYAML::Chunk *>(const Elf_Shdr *)> DumpFn =
            GetDumper(Sec.sh_type)) {
      if (Error E = Add(DumpFn(&Sec)))
        return std::move(E);
      continue;
    }

    // Recognize some special SHT_PROGBITS sections by name.
    if (Sec.sh_type == ELF::SHT_PROGBITS) {
      auto NameOrErr = getUniquedSectionName(&Sec);
      if (!NameOrErr)
        return NameOrErr.takeError();

      if (ELFYAML::StackSizesSection::nameMatches(*NameOrErr)) {
        if (Error E = Add(dumpStackSizesSection(&Sec)))
          return std::move(E);
        continue;
      }
    }

    if (Error E = Add(dumpContentSection(&Sec)))
      return std::move(E);
  }

  return std::move(Ret);
}

template <class ELFT>
Error ELFDumper<ELFT>::dumpSymbols(const Elf_Shdr *Symtab,
                             std::vector<ELFYAML::Symbol> &Symbols) {
  if (!Symtab)
    return Error::success();

  auto StrTableOrErr = Obj.getStringTableForSymtab(*Symtab);
  if (!StrTableOrErr)
    return StrTableOrErr.takeError();
  StringRef StrTable = *StrTableOrErr;

  auto SymtabOrErr = Obj.symbols(Symtab);
  if (!SymtabOrErr)
    return SymtabOrErr.takeError();

  if (Symtab->sh_type == ELF::SHT_SYMTAB) {
    SymTable = *SymtabOrErr;
    SymbolNames.resize(SymTable.size());
  }

  for (const auto &Sym : (*SymtabOrErr).drop_front()) {
    ELFYAML::Symbol S;
    if (auto EC = dumpSymbol(&Sym, Symtab, StrTable, S))
      return EC;
    Symbols.push_back(S);
  }

  return Error::success();
}

template <class ELFT>
Error ELFDumper<ELFT>::dumpSymbol(const Elf_Sym *Sym, const Elf_Shdr *SymTab,
                                  StringRef StrTable, ELFYAML::Symbol &S) {
  S.Type = Sym->getType();
  S.Value = Sym->st_value;
  S.Size = Sym->st_size;
  S.Other = Sym->st_other;
  S.Binding = Sym->getBinding();

  Expected<StringRef> SymbolNameOrErr =
      getUniquedSymbolName(Sym, StrTable, SymTab);
  if (!SymbolNameOrErr)
    return SymbolNameOrErr.takeError();
  S.Name = SymbolNameOrErr.get();

  if (Sym->st_shndx >= ELF::SHN_LORESERVE) {
    S.Index = (ELFYAML::ELF_SHN)Sym->st_shndx;
    return Error::success();
  }

  auto ShdrOrErr = Obj.getSection(*Sym, SymTab, ShndxTable);
  if (!ShdrOrErr)
    return ShdrOrErr.takeError();
  const Elf_Shdr *Shdr = *ShdrOrErr;
  if (!Shdr)
    return Error::success();

  auto NameOrErr = getUniquedSectionName(Shdr);
  if (!NameOrErr)
    return NameOrErr.takeError();
  S.Section = NameOrErr.get();

  return Error::success();
}

template <class ELFT>
template <class RelT>
Error ELFDumper<ELFT>::dumpRelocation(const RelT *Rel, const Elf_Shdr *SymTab,
                                      ELFYAML::Relocation &R) {
  R.Type = Rel->getType(Obj.isMips64EL());
  R.Offset = Rel->r_offset;
  R.Addend = 0;

  auto SymOrErr = Obj.getRelocationSymbol(*Rel, SymTab);
  if (!SymOrErr)
    return SymOrErr.takeError();

  // We have might have a relocation with symbol index 0,
  // e.g. R_X86_64_NONE or R_X86_64_GOTPC32.
  const Elf_Sym *Sym = *SymOrErr;
  if (!Sym)
    return Error::success();

  auto StrTabSec = Obj.getSection(SymTab->sh_link);
  if (!StrTabSec)
    return StrTabSec.takeError();
  auto StrTabOrErr = Obj.getStringTable(**StrTabSec);
  if (!StrTabOrErr)
    return StrTabOrErr.takeError();

  Expected<StringRef> NameOrErr =
      getUniquedSymbolName(Sym, *StrTabOrErr, SymTab);
  if (!NameOrErr)
    return NameOrErr.takeError();
  R.Symbol = NameOrErr.get();

  return Error::success();
}

template <class ELFT>
static unsigned getDefaultShEntSize(ELFYAML::ELF_SHT SecType,
                                    StringRef SecName) {
  switch (SecType) {
  case ELF::SHT_REL:
    return sizeof(typename ELFT::Rel);
  case ELF::SHT_RELA:
    return sizeof(typename ELFT::Rela);
  case ELF::SHT_RELR:
    return sizeof(typename ELFT::Relr);
  case ELF::SHT_DYNAMIC:
    return sizeof(typename ELFT::Dyn);
  default:
    if (SecName == ".debug_str")
      return 1;
    return 0;
  }
}

template <class ELFT>
Error ELFDumper<ELFT>::dumpCommonSection(const Elf_Shdr *Shdr,
                                         ELFYAML::Section &S) {
  // Dump fields. We do not dump the ShOffset field. When not explicitly
  // set, the value is set by yaml2obj automatically.
  S.Type = Shdr->sh_type;
  if (Shdr->sh_flags)
    S.Flags = static_cast<ELFYAML::ELF_SHF>(Shdr->sh_flags);
  if (Shdr->sh_addr)
    S.Address = static_cast<uint64_t>(Shdr->sh_addr);
  S.AddressAlign = Shdr->sh_addralign;

  S.OriginalSecNdx = Shdr - &Sections[0];

  auto NameOrErr = getUniquedSectionName(Shdr);
  if (!NameOrErr)
    return NameOrErr.takeError();
  S.Name = NameOrErr.get();

  if (Shdr->sh_entsize != getDefaultShEntSize<ELFT>(S.Type, S.Name))
    S.EntSize = static_cast<llvm::yaml::Hex64>(Shdr->sh_entsize);

  if (Shdr->sh_link != ELF::SHN_UNDEF) {
    auto LinkSection = Obj.getSection(Shdr->sh_link);
    if (!LinkSection)
      return make_error<StringError>(
          "unable to resolve sh_link reference in section '" + S.Name +
              "': " + toString(LinkSection.takeError()),
          inconvertibleErrorCode());

    NameOrErr = getUniquedSectionName(*LinkSection);
    if (!NameOrErr)
      return NameOrErr.takeError();
    S.Link = NameOrErr.get();
  }

  return Error::success();
}

template <class ELFT>
Error ELFDumper<ELFT>::dumpCommonRelocationSection(
    const Elf_Shdr *Shdr, ELFYAML::RelocationSection &S) {
  if (Error E = dumpCommonSection(Shdr, S))
    return E;

  // Having a zero sh_info field is normal: .rela.dyn is a dynamic
  // relocation section that normally has no value in this field.
  if (!Shdr->sh_info)
    return Error::success();

  auto InfoSection = Obj.getSection(Shdr->sh_info);
  if (!InfoSection)
    return InfoSection.takeError();

  auto NameOrErr = getUniquedSectionName(*InfoSection);
  if (!NameOrErr)
    return NameOrErr.takeError();
  S.RelocatableSec = NameOrErr.get();

  return Error::success();
}

template <class ELFT>
Expected<ELFYAML::StackSizesSection *>
ELFDumper<ELFT>::dumpStackSizesSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::StackSizesSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  ArrayRef<uint8_t> Content = *ContentOrErr;
  DataExtractor Data(Content, Obj.isLE(), ELFT::Is64Bits ? 8 : 4);

  std::vector<ELFYAML::StackSizeEntry> Entries;
  DataExtractor::Cursor Cur(0);
  while (Cur && Cur.tell() < Content.size()) {
    uint64_t Address = Data.getAddress(Cur);
    uint64_t Size = Data.getULEB128(Cur);
    Entries.push_back({Address, Size});
  }

  if (Content.empty() || !Cur) {
    // If .stack_sizes cannot be decoded, we dump it as an array of bytes.
    consumeError(Cur.takeError());
    S->Content = yaml::BinaryRef(Content);
  } else {
    S->Entries = std::move(Entries);
  }

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::AddrsigSection *>
ELFDumper<ELFT>::dumpAddrsigSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::AddrsigSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  ArrayRef<uint8_t> Content = *ContentOrErr;
  DataExtractor::Cursor Cur(0);
  DataExtractor Data(Content, Obj.isLE(), /*AddressSize=*/0);
  std::vector<ELFYAML::YAMLFlowString> Symbols;
  while (Cur && Cur.tell() < Content.size()) {
    uint64_t SymNdx = Data.getULEB128(Cur);
    if (!Cur)
      break;

    Expected<StringRef> SymbolName = getSymbolName(Shdr->sh_link, SymNdx);
    if (!SymbolName || SymbolName->empty()) {
      consumeError(SymbolName.takeError());
      Symbols.emplace_back(
          StringRef(std::to_string(SymNdx)).copy(StringAllocator));
      continue;
    }

    Symbols.emplace_back(*SymbolName);
  }

  if (Cur) {
    S->Symbols = std::move(Symbols);
    return S.release();
  }

  consumeError(Cur.takeError());
  S->Content = yaml::BinaryRef(Content);
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::LinkerOptionsSection *>
ELFDumper<ELFT>::dumpLinkerOptionsSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::LinkerOptionsSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  ArrayRef<uint8_t> Content = *ContentOrErr;
  if (Content.empty() || Content.back() != 0) {
    S->Content = Content;
    return S.release();
  }

  SmallVector<StringRef, 16> Strings;
  toStringRef(Content.drop_back()).split(Strings, '\0');
  if (Strings.size() % 2 != 0) {
    S->Content = Content;
    return S.release();
  }

  S->Options.emplace();
  for (size_t I = 0, E = Strings.size(); I != E; I += 2)
    S->Options->push_back({Strings[I], Strings[I + 1]});

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::DependentLibrariesSection *>
ELFDumper<ELFT>::dumpDependentLibrariesSection(const Elf_Shdr *Shdr) {
  auto DL = std::make_unique<ELFYAML::DependentLibrariesSection>();
  if (Error E = dumpCommonSection(Shdr, *DL))
    return std::move(E);

  Expected<ArrayRef<uint8_t>> ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  ArrayRef<uint8_t> Content = *ContentOrErr;
  if (!Content.empty() && Content.back() != 0) {
    DL->Content = Content;
    return DL.release();
  }

  DL->Libs.emplace();
  for (const uint8_t *I = Content.begin(), *E = Content.end(); I < E;) {
    StringRef Lib((const char *)I);
    DL->Libs->emplace_back(Lib);
    I += Lib.size() + 1;
  }

  return DL.release();
}

template <class ELFT>
Expected<ELFYAML::CallGraphProfileSection *>
ELFDumper<ELFT>::dumpCallGraphProfileSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::CallGraphProfileSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  Expected<ArrayRef<uint8_t>> ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();
  ArrayRef<uint8_t> Content = *ContentOrErr;

  // Dump the section by using the Content key when it is truncated.
  // There is no need to create either "Content" or "Entries" fields when the
  // section is empty.
  if (Content.empty() || Content.size() % 16 != 0) {
    if (!Content.empty())
      S->Content = yaml::BinaryRef(Content);
    return S.release();
  }

  std::vector<ELFYAML::CallGraphEntry> Entries(Content.size() / 16);
  DataExtractor Data(Content, Obj.isLE(), /*AddressSize=*/0);
  DataExtractor::Cursor Cur(0);
  auto ReadEntry = [&](ELFYAML::CallGraphEntry &E) {
    uint32_t FromSymIndex = Data.getU32(Cur);
    uint32_t ToSymIndex = Data.getU32(Cur);
    E.Weight = Data.getU64(Cur);
    if (!Cur) {
      consumeError(Cur.takeError());
      return false;
    }

    Expected<StringRef> From = getSymbolName(Shdr->sh_link, FromSymIndex);
    Expected<StringRef> To = getSymbolName(Shdr->sh_link, ToSymIndex);
    if (From && To) {
      E.From = *From;
      E.To = *To;
      return true;
    }
    consumeError(From.takeError());
    consumeError(To.takeError());
    return false;
  };

  for (ELFYAML::CallGraphEntry &E : Entries) {
    if (ReadEntry(E))
      continue;
    S->Content = yaml::BinaryRef(Content);
    return S.release();
  }

  S->Entries = std::move(Entries);
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::DynamicSection *>
ELFDumper<ELFT>::dumpDynamicSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::DynamicSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto DynTagsOrErr = Obj.template getSectionContentsAsArray<Elf_Dyn>(*Shdr);
  if (!DynTagsOrErr)
    return DynTagsOrErr.takeError();

  for (const Elf_Dyn &Dyn : *DynTagsOrErr)
    S->Entries.push_back({(ELFYAML::ELF_DYNTAG)Dyn.getTag(), Dyn.getVal()});

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::RelocationSection *>
ELFDumper<ELFT>::dumpRelocSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::RelocationSection>();
  if (auto E = dumpCommonRelocationSection(Shdr, *S))
    return std::move(E);

  auto SymTabOrErr = Obj.getSection(Shdr->sh_link);
  if (!SymTabOrErr)
    return SymTabOrErr.takeError();
  const Elf_Shdr *SymTab = *SymTabOrErr;

  if (Shdr->sh_type == ELF::SHT_REL) {
    auto Rels = Obj.rels(*Shdr);
    if (!Rels)
      return Rels.takeError();
    for (const Elf_Rel &Rel : *Rels) {
      ELFYAML::Relocation R;
      if (Error E = dumpRelocation(&Rel, SymTab, R))
        return std::move(E);
      S->Relocations.push_back(R);
    }
  } else {
    auto Rels = Obj.relas(*Shdr);
    if (!Rels)
      return Rels.takeError();
    for (const Elf_Rela &Rel : *Rels) {
      ELFYAML::Relocation R;
      if (Error E = dumpRelocation(&Rel, SymTab, R))
        return std::move(E);
      R.Addend = Rel.r_addend;
      S->Relocations.push_back(R);
    }
  }

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::RelrSection *>
ELFDumper<ELFT>::dumpRelrSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::RelrSection>();
  if (auto E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  if (Expected<ArrayRef<Elf_Relr>> Relrs = Obj.relrs(*Shdr)) {
    S->Entries.emplace();
    for (Elf_Relr Rel : *Relrs)
      S->Entries->emplace_back(Rel);
    return S.release();
  } else {
    // Ignore. We are going to dump the data as raw content below.
    consumeError(Relrs.takeError());
  }

  Expected<ArrayRef<uint8_t>> ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();
  S->Content = *ContentOrErr;
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::RawContentSection *>
ELFDumper<ELFT>::dumpContentSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::RawContentSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  unsigned SecIndex = Shdr - &Sections[0];
  if (SecIndex != 0 || Shdr->sh_type != ELF::SHT_NULL) {
    auto ContentOrErr = Obj.getSectionContents(*Shdr);
    if (!ContentOrErr)
      return ContentOrErr.takeError();
    ArrayRef<uint8_t> Content = *ContentOrErr;
    if (!Content.empty())
      S->Content = yaml::BinaryRef(Content);
  } else {
    S->Size = static_cast<llvm::yaml::Hex64>(Shdr->sh_size);
  }

  if (Shdr->sh_info)
    S->Info = static_cast<llvm::yaml::Hex64>(Shdr->sh_info);
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::SymtabShndxSection *>
ELFDumper<ELFT>::dumpSymtabShndxSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::SymtabShndxSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto EntriesOrErr = Obj.template getSectionContentsAsArray<Elf_Word>(*Shdr);
  if (!EntriesOrErr)
    return EntriesOrErr.takeError();
  for (const Elf_Word &E : *EntriesOrErr)
    S->Entries.push_back(E);
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::NoBitsSection *>
ELFDumper<ELFT>::dumpNoBitsSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::NoBitsSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);
  S->Size = Shdr->sh_size;

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::NoteSection *>
ELFDumper<ELFT>::dumpNoteSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::NoteSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  std::vector<ELFYAML::NoteEntry> Entries;
  ArrayRef<uint8_t> Content = *ContentOrErr;
  while (!Content.empty()) {
    if (Content.size() < sizeof(Elf_Nhdr)) {
      S->Content = yaml::BinaryRef(*ContentOrErr);
      return S.release();
    }

    const Elf_Nhdr *Header = reinterpret_cast<const Elf_Nhdr *>(Content.data());
    if (Content.size() < Header->getSize()) {
      S->Content = yaml::BinaryRef(*ContentOrErr);
      return S.release();
    }

    Elf_Note Note(*Header);
    Entries.push_back(
        {Note.getName(), Note.getDesc(), (llvm::yaml::Hex32)Note.getType()});

    Content = Content.drop_front(Header->getSize());
  }

  S->Notes = std::move(Entries);
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::HashSection *>
ELFDumper<ELFT>::dumpHashSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::HashSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  ArrayRef<uint8_t> Content = *ContentOrErr;
  if (Content.size() % 4 != 0 || Content.size() < 8) {
    S->Content = yaml::BinaryRef(Content);
    return S.release();
  }

  DataExtractor::Cursor Cur(0);
  DataExtractor Data(Content, Obj.isLE(), /*AddressSize=*/0);
  uint32_t NBucket = Data.getU32(Cur);
  uint32_t NChain = Data.getU32(Cur);
  if (Content.size() != (2 + NBucket + NChain) * 4) {
    S->Content = yaml::BinaryRef(Content);
    if (Cur)
      return S.release();
    llvm_unreachable("entries were not read correctly");
  }

  S->Bucket.emplace(NBucket);
  for (uint32_t &V : *S->Bucket)
    V = Data.getU32(Cur);

  S->Chain.emplace(NChain);
  for (uint32_t &V : *S->Chain)
    V = Data.getU32(Cur);

  if (Cur)
    return S.release();
  llvm_unreachable("entries were not read correctly");
}

template <class ELFT>
Expected<ELFYAML::GnuHashSection *>
ELFDumper<ELFT>::dumpGnuHashSection(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::GnuHashSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  unsigned AddrSize = ELFT::Is64Bits ? 8 : 4;
  ArrayRef<uint8_t> Content = *ContentOrErr;
  DataExtractor Data(Content, Obj.isLE(), AddrSize);

  ELFYAML::GnuHashHeader Header;
  DataExtractor::Cursor Cur(0);
  uint32_t NBuckets = Data.getU32(Cur);
  Header.SymNdx = Data.getU32(Cur);
  uint32_t MaskWords = Data.getU32(Cur);
  Header.Shift2 = Data.getU32(Cur);

  // Set just the raw binary content if we were unable to read the header
  // or when the section data is truncated or malformed.
  uint64_t Size = Data.getData().size() - Cur.tell();
  if (!Cur || (Size < MaskWords * AddrSize + NBuckets * 4) ||
      (Size % 4 != 0)) {
    consumeError(Cur.takeError());
    S->Content = yaml::BinaryRef(Content);
    return S.release();
  }

  S->Header = Header;

  S->BloomFilter.emplace(MaskWords);
  for (llvm::yaml::Hex64 &Val : *S->BloomFilter)
    Val = Data.getAddress(Cur);

  S->HashBuckets.emplace(NBuckets);
  for (llvm::yaml::Hex32 &Val : *S->HashBuckets)
    Val = Data.getU32(Cur);

  S->HashValues.emplace((Data.getData().size() - Cur.tell()) / 4);
  for (llvm::yaml::Hex32 &Val : *S->HashValues)
    Val = Data.getU32(Cur);

  if (Cur)
    return S.release();
  llvm_unreachable("GnuHashSection was not read correctly");
}

template <class ELFT>
Expected<ELFYAML::VerdefSection *>
ELFDumper<ELFT>::dumpVerdefSection(const Elf_Shdr *Shdr) {
  typedef typename ELFT::Verdef Elf_Verdef;
  typedef typename ELFT::Verdaux Elf_Verdaux;

  auto S = std::make_unique<ELFYAML::VerdefSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  S->Info = Shdr->sh_info;

  auto StringTableShdrOrErr = Obj.getSection(Shdr->sh_link);
  if (!StringTableShdrOrErr)
    return StringTableShdrOrErr.takeError();

  auto StringTableOrErr = Obj.getStringTable(**StringTableShdrOrErr);
  if (!StringTableOrErr)
    return StringTableOrErr.takeError();

  auto Contents = Obj.getSectionContents(*Shdr);
  if (!Contents)
    return Contents.takeError();

  S->Entries.emplace();

  llvm::ArrayRef<uint8_t> Data = *Contents;
  const uint8_t *Buf = Data.data();
  while (Buf) {
    const Elf_Verdef *Verdef = reinterpret_cast<const Elf_Verdef *>(Buf);
    ELFYAML::VerdefEntry Entry;
    Entry.Version = Verdef->vd_version;
    Entry.Flags = Verdef->vd_flags;
    Entry.VersionNdx = Verdef->vd_ndx;
    Entry.Hash = Verdef->vd_hash;

    const uint8_t *BufAux = Buf + Verdef->vd_aux;
    while (BufAux) {
      const Elf_Verdaux *Verdaux =
          reinterpret_cast<const Elf_Verdaux *>(BufAux);
      Entry.VerNames.push_back(
          StringTableOrErr->drop_front(Verdaux->vda_name).data());
      BufAux = Verdaux->vda_next ? BufAux + Verdaux->vda_next : nullptr;
    }

    S->Entries->push_back(Entry);
    Buf = Verdef->vd_next ? Buf + Verdef->vd_next : nullptr;
  }

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::SymverSection *>
ELFDumper<ELFT>::dumpSymverSection(const Elf_Shdr *Shdr) {
  typedef typename ELFT::Half Elf_Half;

  auto S = std::make_unique<ELFYAML::SymverSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto VersionsOrErr = Obj.template getSectionContentsAsArray<Elf_Half>(*Shdr);
  if (!VersionsOrErr)
    return VersionsOrErr.takeError();
  for (const Elf_Half &E : *VersionsOrErr)
    S->Entries.push_back(E);

  return S.release();
}

template <class ELFT>
Expected<ELFYAML::VerneedSection *>
ELFDumper<ELFT>::dumpVerneedSection(const Elf_Shdr *Shdr) {
  typedef typename ELFT::Verneed Elf_Verneed;
  typedef typename ELFT::Vernaux Elf_Vernaux;

  auto S = std::make_unique<ELFYAML::VerneedSection>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  S->Info = Shdr->sh_info;

  auto Contents = Obj.getSectionContents(*Shdr);
  if (!Contents)
    return Contents.takeError();

  auto StringTableShdrOrErr = Obj.getSection(Shdr->sh_link);
  if (!StringTableShdrOrErr)
    return StringTableShdrOrErr.takeError();

  auto StringTableOrErr = Obj.getStringTable(**StringTableShdrOrErr);
  if (!StringTableOrErr)
    return StringTableOrErr.takeError();

  S->VerneedV.emplace();

  llvm::ArrayRef<uint8_t> Data = *Contents;
  const uint8_t *Buf = Data.data();
  while (Buf) {
    const Elf_Verneed *Verneed = reinterpret_cast<const Elf_Verneed *>(Buf);

    ELFYAML::VerneedEntry Entry;
    Entry.Version = Verneed->vn_version;
    Entry.File =
        StringRef(StringTableOrErr->drop_front(Verneed->vn_file).data());

    const uint8_t *BufAux = Buf + Verneed->vn_aux;
    while (BufAux) {
      const Elf_Vernaux *Vernaux =
          reinterpret_cast<const Elf_Vernaux *>(BufAux);

      ELFYAML::VernauxEntry Aux;
      Aux.Hash = Vernaux->vna_hash;
      Aux.Flags = Vernaux->vna_flags;
      Aux.Other = Vernaux->vna_other;
      Aux.Name =
          StringRef(StringTableOrErr->drop_front(Vernaux->vna_name).data());

      Entry.AuxV.push_back(Aux);
      BufAux = Vernaux->vna_next ? BufAux + Vernaux->vna_next : nullptr;
    }

    S->VerneedV->push_back(Entry);
    Buf = Verneed->vn_next ? Buf + Verneed->vn_next : nullptr;
  }

  return S.release();
}

template <class ELFT>
Expected<StringRef> ELFDumper<ELFT>::getSymbolName(uint32_t SymtabNdx,
                                                   uint32_t SymbolNdx) {
  auto SymtabOrErr = Obj.getSection(SymtabNdx);
  if (!SymtabOrErr)
    return SymtabOrErr.takeError();

  const Elf_Shdr *Symtab = *SymtabOrErr;
  auto SymOrErr = Obj.getSymbol(Symtab, SymbolNdx);
  if (!SymOrErr)
    return SymOrErr.takeError();

  auto StrTabOrErr = Obj.getStringTableForSymtab(*Symtab);
  if (!StrTabOrErr)
    return StrTabOrErr.takeError();
  return getUniquedSymbolName(*SymOrErr, *StrTabOrErr, Symtab);
}

template <class ELFT>
Expected<ELFYAML::Group *> ELFDumper<ELFT>::dumpGroup(const Elf_Shdr *Shdr) {
  auto S = std::make_unique<ELFYAML::Group>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  // Get symbol with index sh_info. This symbol's name is the signature of the group.
  Expected<StringRef> SymbolName = getSymbolName(Shdr->sh_link, Shdr->sh_info);
  if (!SymbolName)
    return SymbolName.takeError();
  S->Signature = *SymbolName;

  auto MembersOrErr = Obj.template getSectionContentsAsArray<Elf_Word>(*Shdr);
  if (!MembersOrErr)
    return MembersOrErr.takeError();

  for (Elf_Word Member : *MembersOrErr) {
    if (Member == llvm::ELF::GRP_COMDAT) {
      S->Members.push_back({"GRP_COMDAT"});
      continue;
    }

    auto SHdrOrErr = Obj.getSection(Member);
    if (!SHdrOrErr)
      return SHdrOrErr.takeError();
    auto NameOrErr = getUniquedSectionName(*SHdrOrErr);
    if (!NameOrErr)
      return NameOrErr.takeError();
    S->Members.push_back({*NameOrErr});
  }
  return S.release();
}

template <class ELFT>
Expected<ELFYAML::MipsABIFlags *>
ELFDumper<ELFT>::dumpMipsABIFlags(const Elf_Shdr *Shdr) {
  assert(Shdr->sh_type == ELF::SHT_MIPS_ABIFLAGS &&
         "Section type is not SHT_MIPS_ABIFLAGS");
  auto S = std::make_unique<ELFYAML::MipsABIFlags>();
  if (Error E = dumpCommonSection(Shdr, *S))
    return std::move(E);

  auto ContentOrErr = Obj.getSectionContents(*Shdr);
  if (!ContentOrErr)
    return ContentOrErr.takeError();

  auto *Flags = reinterpret_cast<const object::Elf_Mips_ABIFlags<ELFT> *>(
      ContentOrErr.get().data());
  S->Version = Flags->version;
  S->ISALevel = Flags->isa_level;
  S->ISARevision = Flags->isa_rev;
  S->GPRSize = Flags->gpr_size;
  S->CPR1Size = Flags->cpr1_size;
  S->CPR2Size = Flags->cpr2_size;
  S->FpABI = Flags->fp_abi;
  S->ISAExtension = Flags->isa_ext;
  S->ASEs = Flags->ases;
  S->Flags1 = Flags->flags1;
  S->Flags2 = Flags->flags2;
  return S.release();
}

template <class ELFT>
static Error elf2yaml(raw_ostream &Out, const object::ELFFile<ELFT> &Obj,
                      std::unique_ptr<DWARFContext> DWARFCtx) {
  ELFDumper<ELFT> Dumper(Obj, std::move(DWARFCtx));
  Expected<ELFYAML::Object *> YAMLOrErr = Dumper.dump();
  if (!YAMLOrErr)
    return YAMLOrErr.takeError();

  std::unique_ptr<ELFYAML::Object> YAML(YAMLOrErr.get());
  yaml::Output Yout(Out);
  Yout << *YAML;

  return Error::success();
}

Error elf2yaml(raw_ostream &Out, const object::ObjectFile &Obj) {
  std::unique_ptr<DWARFContext> DWARFCtx = DWARFContext::create(Obj);
  if (const auto *ELFObj = dyn_cast<object::ELF32LEObjectFile>(&Obj))
    return elf2yaml(Out, *ELFObj->getELFFile(), std::move(DWARFCtx));

  if (const auto *ELFObj = dyn_cast<object::ELF32BEObjectFile>(&Obj))
    return elf2yaml(Out, *ELFObj->getELFFile(), std::move(DWARFCtx));

  if (const auto *ELFObj = dyn_cast<object::ELF64LEObjectFile>(&Obj))
    return elf2yaml(Out, *ELFObj->getELFFile(), std::move(DWARFCtx));

  if (const auto *ELFObj = dyn_cast<object::ELF64BEObjectFile>(&Obj))
    return elf2yaml(Out, *ELFObj->getELFFile(), std::move(DWARFCtx));

  llvm_unreachable("unknown ELF file format");
}
