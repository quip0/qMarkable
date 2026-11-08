import random, datetime, os, sys
random.seed(4)
d=sys.argv[1]; os.makedirs(d+'/days',exist_ok=True)
today=datetime.date.today()
idx=[]
for i in range(300,-1,-1):
    day=today-datetime.timedelta(days=i)
    if random.random()<0.3: continue
    secs=int(random.choice([random.randint(60,1700),random.randint(1800,3500),random.randint(3600,7100),random.randint(7200,15000)]))
    idx.append(f"{day} {secs}")
    if i<3 or True:
        hours=[0]*24
        rem=secs
        for h in random.sample(range(8,23),4):
            take=min(rem,random.randint(0,3600)); hours[h]+=take; rem-=take
        hours[20]+=rem
        docs=[("a1","Calculus II — Lecture notes",int(secs*0.6)),("b2","Organic Chemistry",int(secs*0.3)),("c3","Daily journal",secs-int(secs*0.6)-int(secs*0.3))]
        with open(f"{d}/days/{day}.txt","w") as f:
            f.write("a "+" ".join(map(str,hours))+"\n"+"c "+" ".join(str(h//2) for h in hours)+"\n")
            for doc in docs: f.write("d %s %d %s\n"%(doc[0],doc[2],doc[1]))
open(d+'/index.txt','w').write("# qmarkable 1 tzoff=480 today=%s gap=15\n"%today+"\n".join(idx)+"\n")
