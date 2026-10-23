#pragma once
// Pure checked parser for the address/permission prefix emitted by proc maps.
// No OS state or mapping information is retained here.
#include <charconv>
#include <cstdint>
#include <string_view>

namespace rex::memory::detail {
struct LinuxMapEntry {
  uintptr_t start=0,end=0;
  char perms[5]{};
};
inline bool ProcMapsSpace(char c) noexcept {
  return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||c=='\v';
}
inline bool ParseProcMapsLine(std::string_view line,LinuxMapEntry& out) noexcept {
  out={};
  while(!line.empty()&&ProcMapsSpace(line.front()))line.remove_prefix(1);
  if(line.empty())return false;
  const char* const last=line.data()+line.size();
  uintptr_t begin=0,end=0;
  const auto first=std::from_chars(line.data(),last,begin,16);
  if(first.ec!=std::errc{}||first.ptr==last||*first.ptr!='-')return false;
  const auto second=std::from_chars(first.ptr+1,last,end,16);
  if(second.ec!=std::errc{}||begin>=end||second.ptr==last||!ProcMapsSpace(*second.ptr))return false;
  const char* permissions=second.ptr;
  while(permissions!=last&&ProcMapsSpace(*permissions))++permissions;
  if(last-permissions<4)return false;
  if((permissions[0]!='r'&&permissions[0]!='-')||
     (permissions[1]!='w'&&permissions[1]!='-')||
     (permissions[2]!='x'&&permissions[2]!='-')||
     (permissions[3]!='p'&&permissions[3]!='s')||
     (last-permissions>4&&!ProcMapsSpace(permissions[4])))return false;
  out.start=begin;out.end=end;
  for(unsigned i=0;i<4;++i)out.perms[i]=permissions[i];
  return true;
}
}  // namespace rex::memory::detail
