#pragma once
// Poll even while hidden so a held key cannot become a new press on reopen.
struct MenuKeys {
 bool insertDown=false,escapeDown=false,armed=false;
 void Reset(){armed=false;}
 int Poll(bool insert,bool escape,bool focused,bool visible){
  bool insertPressed=insert&&!insertDown,escapePressed=escape&&!escapeDown;
  insertDown=insert;escapeDown=escape;
  if(!armed){if(!insert&&!escape)armed=true;return 0;}
  if(!focused)return 0;
  if(insertPressed)return 1;
  if(escapePressed&&visible)return 2;
  return 0;
 }
};
