#!/usr/bin/env python3
"""Isolated black-box protocol contract tests; only test ARS 127.0.0.1:7093.
Uses the same TCP+newline JSON that Qt ArsClient uses, without third-party libs.
"""
import os
import socket
import sys
import time
import uuid
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests'))
from hot_cold_end_to_end import Client, check, now_ms


def main():
    if os.environ.get('ARS_TEST_HOST','127.0.0.1') != '127.0.0.1' or os.environ.get('ARS_TEST_PORT') != '7093':
        raise SystemExit('Refusing: this test is pinned to the isolated lab server 127.0.0.1:7093')
    if len(os.environ.get('ARS_API_TOKEN','').encode()) < 32:
        raise SystemExit('ARS_API_TOKEN missing or too short')
    # Ensure a live service before making any changes.
    test = Client()
    id_='CONTRACT_'+uuid.uuid4().hex
    try:
        st=test.call('status')
        check(st['postgres'] and st['hotColdEnabled'] and st['redisCache'] and
              st['inactiveAfterSec'] <= 15,
              'Isolated hot+cold lab detected')
        first = {'id':id_, 'timestamp':now_ms()-10000, 'speed':120.0,
                 'source':'ContractTest', 'features':{'category':'blue','counter':1}}
        second = {'id':id_, 'timestamp':now_ms(), 'speed':260.0,
                  'source':'ContractTest', 'features':{'category':'red','counter':2}}
        a=test.call('save',record=first)
        b=test.call('save',record=second)
        check(a['storage']=='redis' and b['storage']=='redis' and a['sequence']!=b['sequence'],
              'Repeated ID saved as two distinct hot records')
        latest=test.call('latest',id=id_)
        check(latest['found'] and latest['record']['features']['category']=='red',
              'Latest measurement reflects newer event')
        f=[{'field':'features.category','op':'eq','value':'red'},
           {'field':'speed','op':'gte','value':250}]
        matching=test.call('search_hot',mode='records',filters=f,idContains=id_,limit=10,scanBudget=30)
        check(len(matching['items'])==1 and matching['items'][0]['record']['speed']==260.0,
              'Search across RAM records with two field filters')
        latest_search=test.call('search_hot',mode='latest',filters=f,idContains=id_,limit=10,scanBudget=30)
        check(len(latest_search['items'])==1,
              'Latest per-ID RAM search with field filter')
        x=test.call('recent',id=id_,lookbackSec=1800,limit=50)
        check(x['source']=='redis' and len(x['items'])==2 and
              x['items'][0]['receivedAtMs']<=x['items'][1]['receivedAtMs'],
              'Time-indexed 30-minute history ordered by server receipt time')
        future=test.call('recent',id=id_,fromMs=b['receivedAtMs']+60000,
                         toMs=b['receivedAtMs']+120000,limit=10)
        check(not future['items'],'Exact future window returns zero older records')
        x=test.call('history',id=id_,limit=1,timeField='receivedAt',fromMs=0,toMs=-1)
        check(len(x['items'])==1 and x['hasMore'],'Complete history respects page size')
        x2=test.call('history',id=id_,limit=1,timeField='receivedAt',fromMs=0,toMs=-1,
                     afterSequence=x['nextSequence'],untilSequence=x['untilSequence'])
        check(len(x2['items'])==1 and x2['items'][0]['sequence']!=x['items'][0]['sequence'],
              'Sequence cursor retrieves next history page')
        evicted=test.call('delete',id=id_,removeFromDisk=False)
        check(evicted['archivePreserved'],'Redis-only eviction keeps PostgreSQL copy')
        cold=test.call('recent',id=id_,lookbackSec=1800,limit=50)
        check(cold['source']=='postgres' and len(cold['items'])==2,
              'Cold recent history queries SQL directly')
        restored=test.call('hydrate',id=id_)
        if restored.get('status') == 'queued':
            job_id=restored['jobId']
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                state=test.call('recovery_status',jobId=job_id)
                if state['status']=='completed': break
                if state['status']=='failed': raise AssertionError(state.get('error'))
                time.sleep(0.05)
            else: raise AssertionError('Hydration job timed out')
        check(test.call('latest',id=id_)['source']=='redis',
              'Asynchronous explicit hydration brings cold history back to Redis')
        # Validate rejected operations without crashing or mutating data.
        def expect_error(op,**args):
            try: test.call(op,**args)
            except RuntimeError: return True
            return False
        check(expect_error('unknown_op'),'Unknown operations are rejected')
        check(expect_error('history',id=id_,limit=1001),'Oversized history page is rejected')
        check(expect_error('recent',id=id_,lookbackSec=0),'Invalid time range is rejected')
        check(expect_error('search_hot',mode='invalid'),'Invalid search mode is rejected')
        check(expect_error('save',record={'id':id_,'timestamp':0}),
              'Invalid timestamp is rejected')
        gone=test.call('delete',id=id_,removeFromDisk=True)
        check('deleted' in gone and not test.call('latest',id=id_)['found'],
              'Delete ID everywhere removes hot and archive copies')
        print('\nALL LAB CONTRACT TESTS PASSED',flush=True)
    finally:
        try: test.call('delete',id=id_,removeFromDisk=True)
        except Exception as error: print('Cleanup warning:',error,flush=True)
        test.close()

if __name__=='__main__': main()
