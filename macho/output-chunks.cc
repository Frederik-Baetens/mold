#include "mold.h"

namespace mold::macho {

void OutputMachHeader::copy_buf(Context &ctx) {
  MachHeader &mhdr = *(MachHeader *)(ctx.buf + parent.cmd.fileoff + hdr.offset);
  memset(&mhdr, 0, sizeof(mhdr));

  mhdr.magic = 0xfeedfacf;
  mhdr.cputype = CPU_TYPE_X86_64;
  mhdr.cpusubtype = CPU_SUBTYPE_X86_64_ALL;
  mhdr.filetype = MH_EXECUTE;
  mhdr.ncmds = ctx.load_cmd->ncmds;
  mhdr.sizeofcmds = ctx.load_cmd->hdr.size;
  mhdr.flags = MH_TWOLEVEL | MH_NOUNDEFS | MH_DYLDLINK | MH_PIE;
}

static std::pair<std::vector<u8>, i64>
create_load_commands(Context &ctx) {
  std::vector<u8> vec;
  i64 ncmds = 0;

  auto add = [&](auto &x) {
    i64 off = vec.size();
    vec.resize(vec.size() + sizeof(x));
    memcpy(vec.data() + off, &x, sizeof(x));
  };

  // Add a PAGE_ZERO command
  SegmentCommand zero = {};
  zero.cmd = LC_SEGMENT_64;
  zero.cmdsize = sizeof(SegmentCommand);
  strcpy(zero.segname, "__PAGEZERO");
  zero.vmsize = PAGE_ZERO_SIZE;

  add(zero);
  ncmds++;

  // Add LC_SEGMENT_64 comamnds
  for (OutputSegment *seg : ctx.segments) {
    add(seg->cmd);
    ncmds++;

    for (OutputSection *sec : seg->sections)
      if (!sec->is_hidden)
        add(sec->hdr);
  }

  // Add a __LINKEDIT command
  SegmentCommand lnk = {};
  lnk.cmd = LC_SEGMENT_64;
  lnk.cmdsize = sizeof(SegmentCommand);
  strcpy(lnk.segname, "__LINKEDIT");
  lnk.vmaddr = ctx.linkedit->vmaddr;
  lnk.vmsize = align_to(ctx.linkedit->hdr.size, PAGE_SIZE);
  lnk.fileoff = ctx.linkedit->parent.cmd.fileoff + ctx.linkedit->hdr.offset;
  lnk.filesize = ctx.linkedit->hdr.size;
  lnk.maxprot = VM_PROT_READ;
  lnk.initprot = VM_PROT_READ;

  add(lnk);
  ncmds++;

  // Add a LC_SYMTAB command
  SymtabCommand symtab = {};
  symtab.cmd = LC_SYMTAB;
  symtab.cmdsize = sizeof(SymtabCommand);
  symtab.symoff = ctx.linkedit->parent.cmd.fileoff + 
                  ctx.linkedit->hdr.offset + ctx.linkedit->symoff;
  symtab.nsyms = ctx.linkedit->symtab.size() / sizeof(MachSym);
  symtab.stroff = ctx.linkedit->parent.cmd.fileoff + 
                  ctx.linkedit->hdr.offset + ctx.linkedit->stroff;
  symtab.strsize = ctx.linkedit->strtab.size();

//  add(symtab);
//  ncmds++;

  return {vec, ncmds};
}

void OutputLoadCommand::update_hdr(Context &ctx) {
  std::vector<u8> contents;
  std::tie(contents, ncmds) = create_load_commands(ctx);
  hdr.size = contents.size();
}

void OutputLoadCommand::copy_buf(Context &ctx) {
  std::vector<u8> contents;
  std::tie(contents, std::ignore) = create_load_commands(ctx);
  write_vector(ctx.buf + parent.cmd.fileoff + hdr.offset, contents);
}

OutputSegment::OutputSegment(std::string_view name, u32 prot, u32 flags) {
  assert(name.size() <= sizeof(cmd.segname));

  cmd.cmd = LC_SEGMENT_64;
  memcpy(cmd.segname, name.data(), name.size());
  cmd.maxprot = prot;
  cmd.initprot = prot;
  cmd.flags = flags;
}

void OutputSegment::update_hdr(Context &ctx) {
  cmd.cmdsize = sizeof(SegmentCommand);
  cmd.nsects = 0;

  for (OutputSection *sec : sections) {
    if (!sec->is_hidden) {
      cmd.cmdsize += sizeof(MachSection);
      cmd.nsects++;
    }
  }

  i64 fileoff = 0;
  for (OutputSection *sec : sections) {
    sec->update_hdr(ctx);
    fileoff = align_to(fileoff, 1 << sec->hdr.p2align);
    sec->hdr.offset = fileoff;
    fileoff += sec->hdr.size;
  }

  cmd.vmsize = fileoff;
  cmd.filesize = fileoff;
}

void OutputSegment::copy_buf(Context &ctx) {
  for (OutputSection *sec : sections)
    sec->copy_buf(ctx);
}

void OutputLinkEditChunk::update_hdr(Context &ctx) {
  hdr.size = rebase.size() + bind.size() + lazy_bind.size() +
             export_.size() + function_starts.size() + symtab.size() +
             strtab.size();

  symoff = rebase.size() + bind.size() + lazy_bind.size() +
           export_.size() + function_starts.size();
  stroff = symoff + symtab.size();
}

void OutputLinkEditChunk::copy_buf(Context &ctx) {
  i64 off = parent.cmd.fileoff + hdr.offset;

  write_vector(ctx.buf + off, rebase);
  off += rebase.size();

  write_vector(ctx.buf + off, bind);
  off += bind.size();

  write_vector(ctx.buf + off, lazy_bind);
  off += lazy_bind.size();

  write_vector(ctx.buf + off, export_);
  off += lazy_bind.size();

  write_vector(ctx.buf + off, function_starts);
  off += function_starts.size();

  write_vector(ctx.buf + off, symtab);
  off += symtab.size();

  write_vector(ctx.buf + off, strtab);
}

OutputSection::OutputSection(OutputSegment &parent)
  : parent(parent) {
  memcpy(hdr.segname, parent.cmd.segname, sizeof(parent.cmd.segname));
}

TextSection::TextSection(OutputSegment &parent) : OutputSection(parent) {
  strcpy(hdr.sectname, "__text");
  hdr.p2align = __builtin_ctz(8);
  hdr.attr = S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS;

  contents = {
    0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x3d, 0x43, 0x00, 0x00, 0x00,
    0xb0, 0x00, 0xe8, 0x1c, 0x00, 0x00, 0x00, 0x5d, 0xc3, 0x66, 0x2e,
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x90, 0x55,
    0x48, 0x89, 0xe5, 0xe8, 0xd7, 0xff, 0xff, 0xff, 0x31, 0xc0, 0x5d,
    0xc3,
  };

  hdr.size = contents.size();
}


void TextSection::copy_buf(Context &ctx) {
  write_vector(ctx.buf + parent.cmd.fileoff + hdr.offset, contents);
}

StubsSection::StubsSection(OutputSegment &parent) : OutputSection(parent) {
  strcpy(hdr.sectname, "__stubs");
  hdr.p2align = __builtin_ctz(2);
  hdr.type = S_SYMBOL_STUBS;
  hdr.attr = S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS;
  contents = {0x40, 0x7c, 0x25, 0xff, 0x00, 0x00};
  hdr.size = contents.size();
}

void StubsSection::copy_buf(Context &ctx) {
  write_vector(ctx.buf + parent.cmd.fileoff + hdr.offset, contents);
}

StubHelperSection::StubHelperSection(OutputSegment &parent)
  : OutputSection(parent) {
  strcpy(hdr.sectname, "__stub_helper");
  hdr.p2align = __builtin_ctz(4);
  hdr.attr = S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS;

  contents = {
    0x7d, 0x1d, 0x8d, 0x4c, 0x41, 0x00, 0x00, 0x40,
    0x6d, 0x25, 0xff, 0x53, 0x90, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x68, 0xff, 0xe6, 0xe9, 0x00,
    0xff, 0xff,
  };

  hdr.size = contents.size();
}

void StubHelperSection::copy_buf(Context &ctx) {
  write_vector(ctx.buf + parent.cmd.fileoff + hdr.offset, contents);
}

CstringSection::CstringSection(OutputSegment &parent)
  : OutputSection(parent) {
  strcpy(hdr.sectname, "__cstring");
  hdr.p2align = __builtin_ctz(4);
  hdr.type = S_CSTRING_LITERALS;
  hdr.size = sizeof(contents);
}

void CstringSection::copy_buf(Context &ctx) {
  memcpy(ctx.buf + parent.cmd.fileoff + hdr.offset, contents, sizeof(contents));
}

GotSection::GotSection(OutputSegment &parent)
  : OutputSection(parent) {
  strcpy(hdr.sectname, "__cstring");
  hdr.p2align = __builtin_ctz(8);
  hdr.type = S_NON_LAZY_SYMBOL_POINTERS;
  hdr.size = 8;
}

LaSymbolPtrSection::LaSymbolPtrSection(OutputSegment &parent)
  : OutputSection(parent) {
  strcpy(hdr.sectname, "__la_symbol_ptr");
  hdr.p2align = __builtin_ctz(8);
  hdr.type = S_LAZY_SYMBOL_POINTERS;
  contents = {0x94, 0x3f, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
  hdr.size = contents.size();
}

void LaSymbolPtrSection::copy_buf(Context &ctx) {
  write_vector(ctx.buf + parent.cmd.fileoff + hdr.offset, contents);
}

DataSection::DataSection(OutputSegment &parent)
  : OutputSection(parent) {
  strcpy(hdr.sectname, "__data");
  hdr.p2align = __builtin_ctz(8);
  hdr.type = S_LAZY_SYMBOL_POINTERS;

  contents.resize(8);
  hdr.size = contents.size();
}

void DataSection::copy_buf(Context &ctx) {
  write_vector(ctx.buf + parent.cmd.fileoff + hdr.offset, contents);
}

} // namespace mold::macho