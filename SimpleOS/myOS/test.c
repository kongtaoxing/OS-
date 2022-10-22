/*generate 16-bit code*/
__asm__(".code16gcc\n");

/*jump boot code entry*/
__asm__("jmpl $0x0000,$main\n");

/* user defined function to print series of characters terminated by null character */
void printString(const char* pStr) {
     while(*pStr) {
          __asm__ __volatile__ (
               "int $0x10" : : "a"(0x0e00 | *pStr), "b"(0x0007)
          );
          ++pStr;
     }
}

char* scanString(){

     return ;
}

void main() {
     /* calling the printString function passing string as an argument */
     printString("System starting...\n\r");
     printString("System name: SimpleOS\n");
     printString("\rVersion: 1.0\n");
     printString("\rAuthor: xiayunfeng 20281128\n");
     printString("\rPowered by Beijingjiaotong University\n\r");
     while(1) {
          printString("\rxiayunfeng20281128>");
          char* s = scanString();
          if(s == "date")
               printString("time");
     }
     
} 

