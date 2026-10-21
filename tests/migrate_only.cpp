#include "database.hpp"
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc,char** argv){
 try{
  std::filesystem::path root=".";
  for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--data-dir")root=argv[++i];
  std::filesystem::create_directories(root);
  rizoma::database::migrate(root/"morfo.sqlite3");
  std::cout<<"MORFO SQLite migrations complete\n";
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
