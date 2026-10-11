#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "IcsCalendar.h"
#include "IcsEditor.h"
using namespace ics;
struct StrSrc : Source { std::string s; size_t p=0; explicit StrSrc(std::string v):s(std::move(v)){} int read(char*b,int n) override { int k=(int)std::min<size_t>(n,s.size()-p); memcpy(b,s.data()+p,k); p+=k; return k; } };
struct StrSink : Sink { std::string s; bool write(const char*d,size_t n) override { s.append(d,n); return true; } };
#define CHECK(c) EXPECT_TRUE(c)
int32_t D(int y,int m,int d){ return daysFromCivil(y,m,d); }
void moscow(int64_t e,int32_t&day,int16_t&min){ utcIdentity(e+3*3600,day,min); }

TEST(IcsCalendar, ParseExpandAndEdit) {
  // date basics
  CHECK(D(1970,1,1)==0); CHECK(weekdayMon0(D(2026,10,11))==6); // Sunday
  int y; unsigned m,d; civilFromDays(D(2024,2,29),y,m,d); CHECK(y==2024&&m==2&&d==29);
  const char* ics =
   "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
   "BEGIN:VTIMEZONE\r\nTZID:Europe/Moscow\r\nBEGIN:STANDARD\r\nDTSTART:19700101T000000\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
   "BEGIN:VEVENT\r\nUID:a1\r\nDTSTART:20261012T070000Z\r\nDTEND:20261012T080000Z\r\nSUMMARY:Встреча\\, важная\r\nLOCATION:Офис\r\n"
   "BEGIN:VALARM\r\nTRIGGER:-PT15M\r\nSUMMARY:alarm\r\nEND:VALARM\r\nEND:VEVENT\r\n"
   "BEGIN:VEVENT\r\nUID:w1\r\nDTSTART;TZID=Europe/Moscow:20261005T100000\r\nDURATION:PT1H30M\r\nRRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=6\r\nEXDATE;TZID=Europe/Moscow:20261007T100000\r\n"
   "SUMMARY:Очень длинное название которое переносится на несколько стр\r\n ок по правилам RFC\r\nEND:VEVENT\r\n"
   "BEGIN:VEVENT\r\nUID:w1\r\nRECURRENCE-ID;TZID=Europe/Moscow:20261012T100000\r\nDTSTART;TZID=Europe/Moscow:20261012T150000\r\nDTEND;TZID=Europe/Moscow:20261012T160000\r\nSUMMARY:Перенесено\r\nEND:VEVENT\r\n"
   "BEGIN:VEVENT\r\nUID:ad\r\nDTSTART;VALUE=DATE:20261020\r\nDTEND;VALUE=DATE:20261023\r\nSUMMARY:Отпуск\r\nEND:VEVENT\r\n"
   "BEGIN:VEVENT\r\nUID:m2\r\nDTSTART;VALUE=DATE:20260901\r\nRRULE:FREQ=MONTHLY;BYDAY=2TU\r\nSUMMARY:Второй вторник\r\nEND:VEVENT\r\n"
   "BEGIN:VEVENT\r\nUID:y1\r\nDTSTART;VALUE=DATE:19900315\r\nRRULE:FREQ=YEARLY\r\nSUMMARY:ДР\r\nEND:VEVENT\r\n"
   "BEGIN:VEVENT\r\nUID:old\r\nDTSTART:20200101T100000\r\nSUMMARY:Старое\r\nEND:VEVENT\r\n"
   "BEGIN:VTODO\r\nUID:t1\r\nSUMMARY:Купить молоко\r\nDUE;VALUE=DATE:20261013\r\nPRIORITY:1\r\nEND:VTODO\r\n"
   "BEGIN:VTODO\r\nUID:t2\r\nSUMMARY:Готово\r\nSTATUS:COMPLETED\r\nEND:VTODO\r\n"
   "BEGIN:VTODO\r\nUID:t3\r\nSUMMARY:Без срока\r\nEND:VTODO\r\n"
   "END:VCALENDAR\r\n";
  Calendar cal; cal.clear(); cal.setUtcConverter(moscow);
  Calendar::Limits lim; lim.windowStart=D(2026,9,10); lim.windowEnd=D(2027,10,1); cal.setLimits(lim);
  cal.begin(0);
  // feed in small chunks to exercise folding across chunk borders
  std::string s(ics); for(size_t i=0;i<s.size();i+=7) cal.feed(s.data()+i,std::min<size_t>(7,s.size()-i));
  cal.end(); cal.finalize();
    CHECK(cal.events().size()==6); // old dropped
  Occurrence occ[16];
  size_t n=cal.occurrencesOn(D(2026,10,12),occ,16);
  CHECK(n==2); // 10:00 MSK meeting, moved (15:00); weekly instance excluded
  CHECK(n>=2 && occ[0].startMin==600 && strcmp(cal.str(cal.events()[occ[0].event].summary),"Встреча, важная")==0);
  CHECK(n>=2 && occ[1].startMin==900);
  CHECK(!cal.hasEventsOn(D(2026,10,7)));  // exdate
  CHECK(cal.hasEventsOn(D(2026,10,5)) && cal.hasEventsOn(D(2026,10,14)));
  // COUNT=6 on MO,WE from Oct 5: 5,7,12,14,19,21 -> 21st last
  CHECK(cal.hasEventsOn(D(2026,10,21))); CHECK(!cal.hasEventsOn(D(2026,10,26)));
  n=cal.occurrencesOn(D(2026,10,5),occ,16);
  CHECK(n==1 && std::string(cal.str(cal.events()[occ[0].event].summary)).find("строк по")!=std::string::npos);
  // all-day multi day 20..22
  CHECK(cal.hasEventsOn(D(2026,10,22))); CHECK(!cal.hasEventsOn(D(2026,10,23)));
  n=cal.occurrencesOn(D(2026,10,21),occ,16); bool cont=false; for(size_t i=0;i<n;i++) if(occ[i].continued) cont=true; CHECK(cont);
  // second tuesday: Oct 13 2026, Nov 10 2026
  CHECK(cal.hasEventsOn(D(2026,10,13))); CHECK(cal.hasEventsOn(D(2026,11,10))); CHECK(!cal.hasEventsOn(D(2026,11,3)));
  // yearly birthday
  CHECK(cal.hasEventsOn(D(2027,3,15)));
  // todos order: t1 (due), t3 (no due), t2 (completed)
  CHECK(cal.todos().size()==3);
  CHECK(strcmp(cal.str(cal.todos()[0].uid),"t1")==0 && strcmp(cal.str(cal.todos()[1].uid),"t3")==0 && cal.todos()[2].completed);

  // editor: complete t1
  { StrSrc src(s); StrSink out; auto st=editTodo(src,out,"t1",TodoEdit::SetCompleted,"20261011T120000Z");
    CHECK(st==EditStatus::Ok); CHECK(out.s.find("STATUS:COMPLETED\r\nCOMPLETED:20261011T120000Z")!=std::string::npos);
    CHECK(out.s.find("DTEND:20261012T080000Z")!=std::string::npos); // untouched
    CHECK(out.s.find(" ок по правилам RFC\r\n")!=std::string::npos); // folding preserved
    Calendar c2; c2.clear(); c2.begin(0); c2.feed(out.s.data(),out.s.size()); c2.end(); c2.finalize();
    int i=c2.findTodo("t1"); CHECK(i>=0 && c2.todos()[i].completed);
    // and back
    StrSrc src2(out.s); StrSink out2; CHECK(editTodo(src2,out2,"t1",TodoEdit::SetIncomplete,"20261011T130000Z")==EditStatus::Ok);
    Calendar c3; c3.clear(); c3.begin(0); c3.feed(out2.s.data(),out2.s.size()); c3.end(); c3.finalize();
    i=c3.findTodo("t1"); CHECK(i>=0 && !c3.todos()[i].completed);
    CHECK(out2.s.find("COMPLETED:")==std::string::npos);
  }
  { StrSrc src(s); StrSink out; CHECK(editTodo(src,out,"t2",TodoEdit::Delete,"20261011T120000Z")==EditStatus::Ok);
    CHECK(out.s.find("Готово")==std::string::npos); CHECK(out.s.find("Без срока")!=std::string::npos); }
  { StrSrc src(s); StrSink out; CHECK(editTodo(src,out,"nope",TodoEdit::Delete,"x")==EditStatus::NotFound); CHECK(out.s==s); }
  // append with long text to check folding/escaping
  { StrSrc src(s); StrSink out; NewTodo t; t.summary="Позвонить маме; купить хлеб, молоко и ещё очень длинный хвост текста для проверки переноса строк"; t.uid="new-1@crosspoint"; t.dueDay=D(2026,10,15);
    CHECK(appendTodo(&src,out,t,"20261011T120000Z")==EditStatus::Ok);
    CHECK(out.s.size()>15 && out.s.compare(out.s.size()-15,15,"END:VCALENDAR\r\n")==0);
    size_t a=out.s.find("BEGIN:VTODO\r\nUID:new-1"); CHECK(a!=std::string::npos);
    // every physical line <= 75 bytes
    size_t st=a; bool okLen=true; while(st<out.s.size()){ size_t e=out.s.find("\r\n",st); if(e-st>75) okLen=false; st=e+2; } CHECK(okLen);
    Calendar c2; c2.clear(); c2.begin(0); c2.feed(out.s.data(),out.s.size()); c2.end(); c2.finalize();
    int i=c2.findTodo("new-1@crosspoint"); CHECK(i>=0);
    if(i>=0){ CHECK(c2.todos()[i].dueDay==D(2026,10,15)); CHECK(strncmp(c2.str(c2.todos()[i].summary),t.summary,strlen(c2.str(c2.todos()[i].summary)))==0 && strlen(c2.str(c2.todos()[i].summary))>=100); }
  }
  { StrSink out; NewTodo t; t.summary="Первая"; t.uid="u0"; CHECK(appendTodo(nullptr,out,t,"20261011T120000Z")==EditStatus::Ok);
    CHECK(out.s.rfind("BEGIN:VCALENDAR\r\n",0)==0);
    Calendar c2; c2.clear(); c2.begin(0); c2.feed(out.s.data(),out.s.size()); c2.end(); c2.finalize(); CHECK(c2.todos().size()==1); }
  // LF-only file and lowercase
  { const char* lf="BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART:20261011T090000\nSUMMARY:lf\nEND:VEVENT\nEND:VCALENDAR"; Calendar c; c.clear(); c.begin(0); c.feed(lf,strlen(lf)); c.end(); c.finalize(); CHECK(c.events().size()==1); }
  // limits
  { Calendar c; c.clear(); Calendar::Limits l; l.maxEvents=2; c.setLimits(l); c.begin(0); c.feed(ics,strlen(ics)); c.end(); c.finalize(); CHECK(c.truncated()); CHECK(c.events().size()<=2); }
}
