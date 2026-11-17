// Linux comparison through the unchanged production persistence entry point.
#include "persist/ProjectPersistPriv.h"
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs=std::filesystem;
std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);return{std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
 if(argc!=2)return 2;fs::path root=argv[1];fs::create_directories(root);std::string error;const auto file=root/"sidecar",alias=root/"alias",link=root/"symbolic";
 {std::ofstream f(file);f<<"OLD";} chmod(file.c_str(),0664);setxattr(file.c_str(),"user.rawnode","incidental",10,0);
 void*lib=dlopen("libacl.so.1",RTLD_NOW);using Parse=void*(*)(const char*);using Set=int(*)(const char*,int,void*);using Free=int(*)(void*);
 if(lib){auto parse=(Parse)dlsym(lib,"acl_from_text");auto set=(Set)dlsym(lib,"acl_set_file");auto release=(Free)dlsym(lib,"acl_free");void*acl=parse("u::rw-,u:65534:r--,g::r--,m::r--,o::r--");std::cout<<"acl_set="<<set(file.c_str(),0x8000,acl)<<"\n";release(acl);}
 fs::create_hard_link(file,alias);struct stat before{},after{};lstat(file.c_str(),&before);std::cout<<"before uid="<<before.st_uid<<" gid="<<before.st_gid<<" mode="<<std::oct<<(before.st_mode&07777)<<std::dec<<" nlink="<<before.st_nlink<<" inode="<<before.st_ino<<" xattr="<<getxattr(file.c_str(),"user.rawnode",nullptr,0)<<" acl="<<getxattr(file.c_str(),"system.posix_acl_access",nullptr,0)<<"\n";
 const bool ok=persist_detail::replaceFileAtomically(file.string(),"NEW",error);lstat(file.c_str(),&after);std::cout<<"replace="<<ok<<" error="<<error<<" uid="<<after.st_uid<<" gid="<<after.st_gid<<" mode="<<std::oct<<(after.st_mode&07777)<<std::dec<<" nlink="<<after.st_nlink<<" inode="<<after.st_ino<<" xattr="<<getxattr(file.c_str(),"user.rawnode",nullptr,0)<<" acl="<<getxattr(file.c_str(),"system.posix_acl_access",nullptr,0)<<" alias="<<read(alias)<<" file="<<read(file)<<"\n";
 fs::create_symlink(file,link);const bool symlink=persist_detail::replaceFileAtomically(link.string(),"BAD",error);std::cout<<"symlink="<<symlink<<" still_link="<<fs::is_symlink(link)<<" target="<<read(file)<<" error="<<error<<"\n";
 const auto fresh=root/"fresh";const bool create=persist_detail::replaceFileAtomically(fresh.string(),"NEW",error);struct stat created{};lstat(fresh.c_str(),&created);std::cout<<"new="<<create<<" mode="<<std::oct<<(created.st_mode&07777)<<std::dec<<"\n";
 const auto beforeFault=read(file);const bool fault=persist_detail::replaceFileAtomically(file.string(),"PARTIAL",error,persist_detail::FileFault::Write);std::cout<<"fault="<<fault<<" original="<<read(file)<<" error="<<error<<"\n";
 return ok&&read(alias)=="OLD"&&read(file)=="NEW"&&!symlink&&create&&!fault&&beforeFault==read(file)?0:1;
}
