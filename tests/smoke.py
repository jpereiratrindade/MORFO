#!/usr/bin/env python3
# RIZOMA — HTTP smoke/regression gate for local image acquisition and preview.
# Copyright (C) 2026 José Pedro Trindade
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import hashlib
import base64
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib

# Ensure loopback calls bypass system proxies (e.g. Squid/corporate environments)
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))


def png(w=32, h=20, color=(124, 158, 142)):
    signature = b'\x89PNG\r\n\x1a\n'
    def chunk(kind, value):
        body = kind + value
        return struct.pack('!I', len(value)) + body + struct.pack('!I', zlib.crc32(body) & 0xffffffff)
    scanlines = b''.join(b'\x00' + bytes(color)*w for _ in range(h))
    return signature+chunk(b'IHDR',struct.pack('!IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scanlines))+chunk(b'IEND',b'')


def patterned_png(w=517, h=301):
    signature = b'\x89PNG\r\n\x1a\n'
    def chunk(kind, value):
        body = kind + value
        return struct.pack('!I', len(value)) + body + struct.pack('!I', zlib.crc32(body) & 0xffffffff)
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row.extend((x % 251, y % 251, (x + y) % 251))
        rows.append(b'\x00' + bytes(row))
    return signature+chunk(b'IHDR',struct.pack('!IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')


def png_rgb(data):
    """Decode the RGB8 subset emitted for tiles, including PNG row filters."""
    assert data.startswith(b'\x89PNG\r\n\x1a\n')
    offset, compressed, width, height = 8, [], None, None
    while offset < len(data):
        length = struct.unpack('!I', data[offset:offset+4])[0]
        kind, value = data[offset+4:offset+8], data[offset+8:offset+8+length]
        offset += 12 + length
        if kind == b'IHDR':
            width, height, depth, color = struct.unpack('!IIBB', value[:10])
            assert depth == 8 and color == 2
        elif kind == b'IDAT':
            compressed.append(value)
        elif kind == b'IEND':
            break
    raw, stride, prior, pixels, position = zlib.decompress(b''.join(compressed)), width*3, bytearray(width*3), bytearray(), 0
    for _ in range(height):
        filter_type, position = raw[position], position + 1
        scan = bytearray(raw[position:position+stride]); position += stride
        for i in range(stride):
            left = scan[i-3] if i >= 3 else 0
            up = prior[i]
            upper_left = prior[i-3] if i >= 3 else 0
            if filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                estimate = left + up - upper_left
                distances = abs(estimate-left), abs(estimate-up), abs(estimate-upper_left)
                predictor = left if distances[0] <= distances[1] and distances[0] <= distances[2] else up if distances[1] <= distances[2] else upper_left
            else:
                assert filter_type == 0
                predictor = 0
            scan[i] = (scan[i] + predictor) & 0xff
        pixels.extend(scan); prior = scan
    return width, height, bytes(pixels)


def progressive_png(w=7001, h=7001, color=(72, 111, 83)):
    """Build a highly compressible large fixture without a full raster buffer."""
    signature = b'\x89PNG\r\n\x1a\n'
    def chunk(kind, value):
        body = kind + value
        return struct.pack('!I', len(value)) + body + struct.pack('!I', zlib.crc32(body) & 0xffffffff)
    compressor = zlib.compressobj(level=1)
    row = b'\x00' + bytes(color)*w
    compressed = []
    for _ in range(h):
        block = compressor.compress(row)
        if block:
            compressed.append(block)
    compressed.append(compressor.flush())
    return signature+chunk(b'IHDR',struct.pack('!IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',b''.join(compressed))+chunk(b'IEND',b'')


def candidate_png(w=64, h=64):
    signature = b'\x89PNG\r\n\x1a\n'
    def chunk(kind, value):
        body = kind + value
        return struct.pack('!I', len(value)) + body + struct.pack('!I', zlib.crc32(body) & 0xffffffff)
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            value = 245 if 8 <= y < 52 and 29 <= x < 32 else 25
            row.extend((value, value, value))
        rows.append(b'\x00' + bytes(row))
    return signature+chunk(b'IHDR',struct.pack('!IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')


def multi_candidate_png(w=80, h=80):
    signature = b'\x89PNG\r\n\x1a\n'
    def chunk(kind, value):
        body = kind + value
        return struct.pack('!I', len(value)) + body + struct.pack('!I', zlib.crc32(body) & 0xffffffff)
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            is_r1 = (10 <= y < 60 and 12 <= x < 16)
            is_r2 = (10 <= y < 60 and 56 <= x < 60)
            value = 245 if (is_r1 or is_r2) else 25
            row.extend((value, value, value))
        rows.append(b'\x00' + bytes(row))
    return signature+chunk(b'IHDR',struct.pack('!IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')


def req(url, method='GET', data=None, headers=None):
    request = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, response.read(), response.headers
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers


def main():
    exe, web = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='rizoma-test-') as temp:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        proc = subprocess.Popen([exe,'--data-dir',temp,'--web-dir',web,'--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        base=f'http://127.0.0.1:{port}'
        try:
            for _ in range(80):
                try:
                    if req(base+'/api/v1/health')[0] == 200: break
                except OSError:
                    time.sleep(.08)
            else: raise AssertionError('Server did not start')
            status,body,_=req(base+'/')
            assert status==200 and b'MORFO' in body

            # 1. Access without authentication must be rejected
            status,body,_=req(base+'/api/v1/auth/me')
            assert status==200 and json.loads(body)['authenticated'] is False
            status,body,_=req(base+'/api/v1/images')
            assert status==401,(status,body)

            # 2. Registration / Code request: only @embrapa.br allowed
            status,body,_=req(base+'/api/v1/auth/request-code','POST',json.dumps({'email':'invasor@gmail.com'}).encode(),{'Content-Type':'application/json'})
            assert status==400 and b'@embrapa.br' in body,(status,body)
            status,body,_=req(base+'/api/v1/auth/request-code','POST',json.dumps({'email':'alvo@embrapa.com'}).encode(),{'Content-Type':'application/json'})
            assert status==400,(status,body)

            # 3. Valid institutional email generates OTP code
            email = 'pesquisador.campo@embrapa.br'
            status,body,_=req(base+'/api/v1/auth/request-code','POST',json.dumps({'email':email}).encode(),{'Content-Type':'application/json'})
            assert status==200,(status,body)
            resp_obj = json.loads(body)
            otp_code = resp_obj.get('dev_code')
            assert otp_code and len(otp_code)==6 and otp_code.isdigit(),(status,body)

            # 4. Invalid OTP rejected
            status,body,_=req(base+'/api/v1/auth/verify-code','POST',json.dumps({'email':email,'code':'999999'}).encode(),{'Content-Type':'application/json'})
            assert status==401,(status,body)

            # 5. Correct OTP establishes session
            status,body,hdrs=req(base+'/api/v1/auth/verify-code','POST',json.dumps({'email':email,'code':otp_code}).encode(),{'Content-Type':'application/json'})
            assert status==200,(status,body)
            auth_info = json.loads(body)
            token = auth_info['session_token']
            assert len(token)==64
            assert auth_info['user']['email']==email

            # 6. Verify authenticated session
            auth_headers = {'Cookie': f'morfo_session={token}'}
            status,body,_=req(base+'/api/v1/auth/me',headers=auth_headers)
            assert status==200 and json.loads(body)['authenticated'] is True
            assert json.loads(body)['user']['email']==email

            # Irregular dimensions cross tile boundaries and exercise EXP-004 edge tiles.
            data=patterned_png()
            upload=base+'/api/v1/images?filename=campo.png&sample_id=P01-T2&capture_date=2026-10-14'
            headers={'Content-Type':'application/octet-stream','X-Morfo-Client':'web-v0.2.0',**auth_headers}
            status,body,_=req(upload,'POST',data,headers)
            assert status==201,(status,body)
            obj=json.loads(body)['data']
            assert obj['width']==517 and obj['height']==301 and obj['sha256']==hashlib.sha256(data).hexdigest(),obj
            assert obj['scientific_status']=='NOT_ANALYZED'
            assert obj['user_email']==email,obj
            uid=obj['id']
            for kind,magic in [('original',b'\x89PNG'),('preview',b'\x89PNG'),('grayscale',b'\x89PNG')]:
                status,payload,_=req(base+'/api/v1/images/'+uid+'/'+kind,headers=auth_headers)
                assert status==200 and payload.startswith(magic),(kind,status)
            status,body,_=req(base+'/api/v1/images/'+uid+'/pyramid',headers=auth_headers)
            pyramid=json.loads(body)
            assert status==200 and pyramid['schema']=='rizoma.image-pyramid.v1'
            assert [(level['width'],level['height']) for level in pyramid['levels']]==[(130,76),(259,151),(517,301)],pyramid
            assert all('/derived/' not in level['url_template'] and '/blobs/' not in level['url_template'] for level in pyramid['levels'])
            status,tile,headers_tile=req(base+'/api/v1/images/'+uid+'/tiles/0/2/1.png',headers=auth_headers)
            assert status==200 and headers_tile.get('Content-Type')=='image/png' and tile.startswith(b'\x89PNG'),(status,headers_tile.get('Content-Type'),tile[:24])
            assert struct.unpack('!II',tile[16:24])==(5,45)
            tile_width,tile_height,tile_pixels=png_rgb(tile)
            assert (tile_width,tile_height)==(5,45) and tile_pixels[:3]==bytes((512 % 251,256 % 251,(512+256) % 251))
            status,_,_=req(base+'/api/v1/images/'+uid+'/tiles/0/3/1.png',headers=auth_headers)
            assert status==404,status
            # A real, minimal 8x6 JPEG fixture, independent from imaging Python packages.
            jpeg = base64.b64decode(
                '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAGAAgDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDn6KKK7DkP/9k='
            )
            status,body,_=req(base+'/api/v1/images?filename=teste.jpg','POST',jpeg,headers)
            assert status==201,(status,body)
            item=json.loads(body)['data']
            assert item['format']=='jpeg' and item['width']==8 and item['height']==6 and item['user_email']==email
            # A real, minimal 8x6 RGB TIFF fixture, independent from imaging Python packages.
            tiff = base64.b64decode(
                'SUkqAAgAAAAKAAABBAABAAAACAAAAAEBBAABAAAABgAAAAIBAwADAAAAhgAAAAMBAwABAAAAAQAAAAYB'
                'AwABAAAAAgAAABEBBAABAAAAjAAAABUBAwABAAAAAwAAABYBBAABAAAABgAAABcBBAABAAAAkAAAABwB'
                'AwABAAAAAQAAAAAAAAAIAAgACABklshklshklshklshklshklshklshklshklshklshklshklshklshkl'
                'shklshklshklshklshklshklshklshklshklshklshklshklshklshklshklshklshklshklshklshkls'
                'hklshklshklshklshklshklshklshklshklshklshklshklshklshklshklsg='
            )
            status,body,_=req(base+'/api/v1/images?filename=scanner_raiz.tif&sample_id=P01-RAIZ','POST',tiff,headers)
            assert status==201,(status,body)
            item_tif=json.loads(body)['data']
            assert item_tif['format']=='tiff' and item_tif['width']==8 and item_tif['height']==6 and item_tif['user_email']==email
            status,payload,resp_hdrs=req(base+'/api/v1/images/'+item_tif['id']+'/original',headers=auth_headers)
            assert status==200 and payload.startswith(b'II*\x00'),status
            assert resp_hdrs.get('Content-Type')=='image/tiff'
            status,payload,resp_hdrs=req(base+'/api/v1/images/'+item_tif['id']+'/original-view',headers=auth_headers)
            assert status==200 and payload.startswith(b'\x89PNG'),status
            assert resp_hdrs.get('Content-Type')=='image/png'
            status,payload,_=req(base+'/api/v1/images/'+item_tif['id']+'/preview',headers=auth_headers)
            assert status==200 and payload.startswith(b'\x89PNG'),status
            status,body,_=req(base+'/api/v1/images',headers=auth_headers)
            assert status==200 and len(json.loads(body)['data'])==3
            status,body,_=req(base+'/api/v1/export/manifest.csv',headers=auth_headers)
            assert status==200 and b'P01-T2' in body and b'P01-RAIZ' in body and b'NOT_ANALYZED' in body
            assert email.encode() in body
            status,body,_=req(upload,'POST',data,headers)
            assert status==409,(status,body)
            status,body,_=req(upload,'POST',b'<script>not an image</script>',headers)
            assert status==415,(status,body)
            status,body,_=req(upload,'POST',data,{'Content-Type':'application/octet-stream',**auth_headers})
            assert status==403,(status,body)
            status,body,_=req(upload,'POST',data,{**headers,'Origin':'http://malicious-attacker.com'})
            assert status==403,(status,body)
            status,body,_=req(upload,'POST',data,{**headers,'Origin':f'http://127.0.0.1:{port}'})
            assert status==409,(status,body)
            status,body,_=req(base+'/api/v1/images/../../etc/passwd',headers=auth_headers)
            assert status in (400,404),(status,body)

            # Integrated experimental analysis -> suggestion -> append-only human review.
            candidate = candidate_png()
            candidate_upload = base+'/api/v1/images?filename=candidato-raiz.png&sample_id=ANALISE-01'
            status,body,_=req(candidate_upload,'POST',candidate,headers)
            assert status==201,(status,body)
            analysis_image_id=json.loads(body)['data']['id']
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis','POST',b'',auth_headers)
            assert status==201,(status,body)
            analysis=json.loads(body)['data']
            first_run_id=analysis['id']
            assert analysis['source_sha256']==hashlib.sha256(candidate).hexdigest()
            assert analysis['algorithm_version']=='0.1.0-experimental'
            assert analysis['regions'] and analysis['regions'][0]['suggestion'] in ('ROOT','UNKNOWN'),analysis
            region=analysis['regions'][0]
            review_url=base+f"/api/v1/analysis-regions/{region['id']}/reviews"
            review_headers={'Content-Type':'application/json',**auth_headers}
            first_review={'action':'CONFIRM','label':region['suggestion'],'justification':'Confirmação "visual" da hipótese experimental','expected_version':0}
            status,body,_=req(review_url,'POST',json.dumps(first_review).encode(),review_headers)
            assert status==201 and len(json.loads(body)['data'])==1,(status,body)
            # Stale expected_version must not overwrite the prior event.
            stale={'action':'CORRECT','label':'ARTIFACT','justification':'concorrente','expected_version':0}
            status,body,_=req(review_url,'POST',json.dumps(stale).encode(),review_headers)
            assert status==409,(status,body)
            correction={'action':'CORRECT','label':'ARTIFACT','justification':'Correção preservando a interpretação anterior','expected_version':1}
            status,body,_=req(review_url,'POST',json.dumps(correction).encode(),review_headers)
            history=json.loads(body)['data']
            assert status==201 and [event['version'] for event in history]==[1,2]
            assert [event['label'] for event in history]==[region['suggestion'],'ARTIFACT']
            suspension={'action':'SUSPEND','label':'UNKNOWN','justification':'Evidência insuficiente; suspender julgamento','expected_version':2}
            status,body,_=req(review_url,'POST',json.dumps(suspension).encode(),review_headers)
            history=json.loads(body)['data']
            assert status==201 and [event['version'] for event in history]==[1,2,3]
            assert history[0]['justification']=='Confirmação "visual" da hipótese experimental'
            db=sqlite3.connect(Path(temp)/'morfo.sqlite3')
            assert db.execute("SELECT COUNT(*) FROM analysis_judgments WHERE region_id=?",(region['id'],)).fetchone()[0]==1
            assert db.execute("SELECT COUNT(*) FROM review_events WHERE region_id=?",(region['id'],)).fetchone()[0]==3
            try:
                db.execute("UPDATE review_events SET label='ROOT' WHERE region_id=?",(region['id'],))
                raise AssertionError('append-only review update unexpectedly succeeded')
            except sqlite3.IntegrityError:
                pass
            db.close()
            # A later execution remains separate and the earlier run stays addressable.
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis','POST',b'',auth_headers)
            assert status==201 and json.loads(body)['data']['id']!=first_run_id,(status,body)
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis-runs',headers=auth_headers)
            runs=json.loads(body)['data']
            assert status==200 and len(runs)==2 and any(run['id']==first_run_id for run in runs),(status,body)
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis?run_id={first_run_id}',headers=auth_headers)
            assert status==200 and json.loads(body)['data']['regions'][0]['version']==3,(status,body)

            # Test the 3 learning levels (Bayesian, Tabular SGD, and Visual k-NN)
            for strat in ['BAYESIAN_CALIBRATED', 'TABULAR_INCREMENTAL', 'VISUAL_KNN']:
                status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis?algorithm={strat}','POST',b'',auth_headers)
                assert status==201,(status,body)
                run_data = json.loads(body)['data']
                assert run_data['algorithm']==strat, run_data
                assert run_data['algorithm_version']=='learning-v1.0', run_data
                assert run_data['regions'][0]['judgment_evidence']['learning']['strategy']==strat, run_data

            db=sqlite3.connect(Path(temp)/'morfo.sqlite3')
            assert db.execute("SELECT COUNT(*) FROM learning_models").fetchone()[0]>=1
            assert db.execute("SELECT COUNT(*) FROM learning_exemplars").fetchone()[0]>=1
            db.close()

            # Real-time online recalibration of pending regions upon human feedback
            multi_cand = multi_candidate_png()
            multi_upload = base+'/api/v1/images?filename=multi-raizes.png&sample_id=RECALIB-01'
            status,body,_ = req(multi_upload,'POST',multi_cand,headers)
            assert status==201, (status,body)
            multi_img_id = json.loads(body)['data']['id']
            status,body,_ = req(base+f'/api/v1/images/{multi_img_id}/analysis?algorithm=TABULAR_INCREMENTAL','POST',b'',auth_headers)
            assert status==201, (status,body)
            multi_analysis = json.loads(body)['data']
            assert len(multi_analysis['regions']) >= 2, multi_analysis
            r1 = multi_analysis['regions'][0]
            r2 = multi_analysis['regions'][1]
            r1_review_url = base+f"/api/v1/analysis-regions/{r1['id']}/reviews"
            r1_confirm = {'action':'CONFIRM','label':r1['suggestion'],'justification':'Aprovando R1 para recalibrar modelo online','expected_version':0}
            status,body,_ = req(r1_review_url,'POST',json.dumps(r1_confirm).encode(),review_headers)
            assert status==201, (status,body)
            rev_resp = json.loads(body)
            assert rev_resp.get('recalibrated_count', 0) >= 1, rev_resp

            # Fetch analysis again: r1 must be reviewed, r2 must be recalibrated and remain pending (version 0)
            status,body,_ = req(base+f'/api/v1/images/{multi_img_id}/analysis',headers=auth_headers)
            assert status==200, (status,body)
            updated_analysis = json.loads(body)['data']
            up_r1 = [r for r in updated_analysis['regions'] if r['id'] == r1['id']][0]
            up_r2 = [r for r in updated_analysis['regions'] if r['id'] == r2['id']][0]
            assert up_r1['version'] == 1 and up_r1['review'] is not None
            assert up_r2['version'] == 0 and up_r2['review'] is None
            assert up_r2['judgment_evidence'].get('recalibrated') is True, up_r2

            # Check database: r2 has 2 judgments (initial + recalibrated), maintaining auditability
            db=sqlite3.connect(Path(temp)/'morfo.sqlite3')
            assert db.execute("SELECT COUNT(*) FROM analysis_judgments WHERE region_id=?", (r2['id'],)).fetchone()[0] == 2
            db.close()

            # EXP-004 gate: acquisition and review exceed the former 12 MP ceiling.
            large = progressive_png()
            large_upload = base+'/api/v1/images?filename=campo-progressivo-7001.png&sample_id=EXP-004-GATE'
            status,body,_=req(large_upload,'POST',large,headers)
            assert status==201,(status,body)
            large_item=json.loads(body)['data']
            large_id=large_item['id']
            assert large_item['width']==7001 and large_item['height']==7001,(status,body)
            assert large_item['sha256']==hashlib.sha256(large).hexdigest(),large_item
            large_blob=Path(temp)/'blobs'/large_item['sha256'][:2]/(large_item['sha256']+'.png')
            assert large_blob.read_bytes()==large
            large_tiles=list((Path(temp)/'derived'/large_id/'pyramid').glob('L*/*.png'))
            assert len(large_tiles)==1050 and not (Path(temp)/'derived'/large_id/'pyramid'/'.rowband.tmp').exists(),len(large_tiles)
            status,body,_=req(base+f'/api/v1/images/{large_id}/pyramid',headers=auth_headers)
            large_pyramid=json.loads(body)
            assert status==200 and large_pyramid['levels'][0]['width']<=256 and large_pyramid['levels'][-1]['width']==7001,(status,body)
            status,tile,tile_headers=req(base+f'/api/v1/images/{large_id}/tiles/0/27/27.png',headers=auth_headers)
            assert status==200 and tile_headers.get('Content-Type')=='image/png' and struct.unpack('!II',tile[16:24])==(89,89),(status,tile[:24])
            status,body,_=req(base+f'/api/v1/images/{large_id}/analysis','POST',b'',auth_headers)
            assert status==422 and b'12 megapixels' in body,(status,body)

            # MORFO thematic profile: backend EXP-003 runs, records, isolates derivatives.
            status, body, _ = req(base+'/api/v1/profiles')
            assert status == 200 and b'roots-exp003' in body and b'PLANNED' in body
            profile_url = base+f'/api/v1/images/{analysis_image_id}/profiles/roots-exp003'
            status, body, _ = req(profile_url, 'POST', b'', auth_headers)
            assert status == 201, (status, body)
            profile = json.loads(body)['data']
            assert profile['scientific_status'] == 'NOT_VALIDATED', profile
            assert profile['biological_identity'] == 'UNKNOWN', profile
            profile_id = profile['id']
            status, data, _ = req(profile_url+'/'+profile_id+'/mask', headers=auth_headers)
            assert status == 200 and data.startswith(b'\x89PNG'), (status, data[:16])
            status, data, _ = req(profile_url+'/'+profile_id+'/report', headers=auth_headers)
            assert status == 200 and json.loads(data)['classification'] == 'ANOMALY_CANDIDATE'
            status, data, _ = req(profile_url, headers=auth_headers)
            assert status == 200 and profile_id in data.decode(), (status, data)
            status, data, _ = req(base+f'/api/v1/images/{large_id}/profiles/roots-exp003', 'POST', b'', auth_headers)
            assert status == 422 and b'12 MP' in data, (status, data)

            # 7. Multi-user isolation verification (User 2)
            email2 = 'pesquisador2@embrapa.br'
            status,body,_=req(base+'/api/v1/auth/request-code','POST',json.dumps({'email':email2}).encode(),{'Content-Type':'application/json'})
            assert status==200
            otp2 = json.loads(body)['dev_code']
            status,body,_=req(base+'/api/v1/auth/verify-code','POST',json.dumps({'email':email2,'code':otp2}).encode(),{'Content-Type':'application/json'})
            assert status==200
            token2 = json.loads(body)['session_token']
            auth_headers2 = {'Cookie': f'morfo_session={token2}'}

            # User 2 catalog must be completely isolated (0 images initially)
            status,body,_=req(base+'/api/v1/images',headers=auth_headers2)
            assert status==200 and len(json.loads(body)['data'])==0, (status,body)

            # User 2 cannot access or probe User 1's images (returns 404)
            status,body,_=req(base+'/api/v1/images/'+uid,headers=auth_headers2)
            assert status==404, (status,body)
            status,body,_=req(base+'/api/v1/images/'+uid+'/preview',headers=auth_headers2)
            assert status==404, (status,body)
            status,body,_=req(base+'/api/v1/images/'+uid+'/original',headers=auth_headers2)
            assert status==404, (status,body)
            status,body,_=req(base+'/api/v1/images/'+uid+'/pyramid',headers=auth_headers2)
            assert status==404, (status,body)
            status,body,_=req(base+'/api/v1/images/'+uid+'/tiles/0/0/0.png',headers=auth_headers2)
            assert status==404, (status,body)
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis',headers=auth_headers2)
            assert status==404,(status,body)
            status,body,_=req(review_url,headers=auth_headers2)
            assert status==404,(status,body)
            status,body,_=req(review_url,'POST',json.dumps({'action':'SUSPEND','label':'UNKNOWN','justification':'x','expected_version':3}).encode(),{'Content-Type':'application/json',**auth_headers2})
            assert status==404,(status,body)

            # User 2 manifest is also isolated and does not leak User 1's entries
            status,body,_=req(base+'/api/v1/export/manifest.csv',headers=auth_headers2)
            assert status==200 and email.encode() not in body and b'P01-T2' not in body

            # User 2 uploads their own image
            data2 = png(color=(80, 120, 200))
            upload2 = base+'/api/v1/images?filename=raiz_solo.png&sample_id=P02-SOLO&capture_date=2026-10-15'
            # The legacy RIZOMA client header remains accepted for compatibility.
            headers2 = {'Content-Type':'application/octet-stream','X-Rizoma-Client':'web-v0.2.0',**auth_headers2}
            status,body,_=req(upload2,'POST',data2,headers2)
            assert status==201, (status,body)
            obj2 = json.loads(body)['data']
            uid2 = obj2['id']
            assert obj2['user_email']==email2

            # User 2 sees only their 1 image
            status,body,_=req(base+'/api/v1/images',headers=auth_headers2)
            assert status==200 and len(json.loads(body)['data'])==1

            # User 1 still sees only their 6 images and cannot access User 2's image
            status,body,_=req(base+'/api/v1/images',headers=auth_headers)
            assert status==200 and len(json.loads(body)['data'])==6
            status,body,_=req(base+'/api/v1/images/'+uid2,headers=auth_headers)
            assert status==404, (status,body)
            status,body,_=req(base+'/api/v1/images/'+uid2+'/preview',headers=auth_headers)
            assert status==404, (status,body)

            # 8. Test Logout and subsequent rejection
            status,body,_=req(base+'/api/v1/auth/logout','POST',headers=auth_headers)
            assert status==200
            status,body,_=req(base+'/api/v1/auth/me',headers=auth_headers)
            assert status==200 and json.loads(body)['authenticated'] is False
            status,body,_=req(base+'/api/v1/images',headers=auth_headers)
            assert status==401

            # 9. Re-authenticate User 1 for persistence across restart
            status,body,_=req(base+'/api/v1/auth/request-code','POST',json.dumps({'email':email}).encode(),{'Content-Type':'application/json'})
            assert status==200
            new_code = json.loads(body)['dev_code']
            status,body,_=req(base+'/api/v1/auth/verify-code','POST',json.dumps({'email':email,'code':new_code}).encode(),{'Content-Type':'application/json'})
            assert status==200
            new_token = json.loads(body)['session_token']
            restarted_headers1 = {'Cookie': f'morfo_session={new_token}'}

            # Persistence after an actual restart for both users
            proc.terminate(); proc.wait(timeout=6)
            proc=subprocess.Popen([exe,'--host','127.0.0.1','--data-dir',temp,'--web-dir',web,'--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            for _ in range(80):
                try:
                    status,body,_=req(base+'/api/v1/images',headers=restarted_headers1)
                    if status==200: break
                except OSError: time.sleep(.08)
            else: raise AssertionError('Restart failed')
            assert len(json.loads(body)['data'])==6
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis?run_id={first_run_id}',headers=restarted_headers1)
            recovered=json.loads(body)['data']
            assert status==200 and recovered['regions'][0]['review']['label']=='UNKNOWN' and recovered['regions'][0]['review']['action']=='SUSPEND',(status,body)
            status,body,_=req(review_url,headers=restarted_headers1)
            assert status==200 and len(json.loads(body)['data'])==3,(status,body)

            # Check User 2's isolated catalog after restart
            status,body,_=req(base+'/api/v1/images',headers=auth_headers2)
            assert status==200 and len(json.loads(body)['data'])==1
            status,body,_=req(base+'/api/v1/images/'+uid2,headers=auth_headers2)
            assert status==200

            # 10. CRUD test for Images and Analyses
            # Patch Image metadata
            patch_data = json.dumps({'sample_id':'P01-EDITED','capture_date':'2026-10-20','filename':'campo_editado.png'}).encode()
            status,body,_=req(base+'/api/v1/images/'+uid,'PATCH',patch_data,{'Content-Type':'application/json',**restarted_headers1})
            assert status==200,(status,body)
            img_up = json.loads(body)['data']
            assert img_up['sample_id']=='P01-EDITED' and img_up['capture_date']=='2026-10-20' and img_up['filename']=='campo_editado.png'

            # Patch Analysis Run status and notes
            patch_run = json.dumps({'status':'COMPLETED','notes':'Validado em campo pelo pesquisador.'}).encode()
            status,body,_=req(base+'/api/v1/analysis-runs/'+first_run_id,'PATCH',patch_run,{'Content-Type':'application/json',**restarted_headers1})
            assert status==200,(status,body)
            run_up = json.loads(body)['data']
            assert run_up['status']=='COMPLETED' and run_up['notes']=='Validado em campo pelo pesquisador.' and run_up['completed_at']

            # Create a fresh analysis run to test batch confirm on pending regions
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis','POST',b'',restarted_headers1)
            assert status==201,(status,body)
            fresh_run_id = json.loads(body)['data']['id']
            # Batch confirm all pending regions
            status,body,_=req(base+'/api/v1/analysis-runs/'+fresh_run_id+'/confirm-all','POST',json.dumps({'justification':'Aprovação em lote no teste de fumaça'}).encode(),{'Content-Type':'application/json',**restarted_headers1})
            assert status==200,(status,body)
            conf_res = json.loads(body)
            assert conf_res['status']=='ok' and conf_res['confirmed_count'] > 0
            # Get analysis and verify all regions have reviews recorded
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis?run_id={fresh_run_id}',headers=restarted_headers1)
            an_data = json.loads(body)['data']
            assert all(r['review'] is not None and r['review']['action']=='CONFIRM' for r in an_data['regions'])
            stats = an_data['statistics']
            assert stats['total_regions'] == len(an_data['regions'])
            assert stats['pending_count'] == 0
            assert stats['reviewed_count'] == stats['total_regions']

            # Test Analysis Statistics CSV export
            status,csv_body,csv_headers = req(base+f'/api/v1/images/{analysis_image_id}/analysis-export.csv?run_id={fresh_run_id}',headers=restarted_headers1)
            assert status == 200, (status, csv_body)
            assert b'MORFO - Relatorio Cientifico e Estatisticas Radiculares' in csv_body
            assert b'SUMARIO ESTATISTICO RADICULAR:' in csv_body
            assert b'Densidade Radicular' in csv_body
            assert b'ordinal,x,y,width,height,area_pixels,label' in csv_body

            # Soft Delete Analysis Run
            status,body,_=req(base+'/api/v1/analysis-runs/'+first_run_id,'DELETE',headers=restarted_headers1)
            assert status==200 and json.loads(body)['data']['deleted'] is True
            # Verified run is not in analysis-runs list
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis-runs',headers=restarted_headers1)
            runs_list = json.loads(body)['data']
            assert all(r['id']!=first_run_id for r in runs_list)
            # Verified GET analysis for deleted run returns data: null
            status,body,_=req(base+f'/api/v1/images/{analysis_image_id}/analysis?run_id={first_run_id}',headers=restarted_headers1)
            assert status==200 and json.loads(body)['data'] is None,(status,body)

            # Soft Delete Image
            status,body,_=req(base+'/api/v1/images/'+uid,'DELETE',headers=restarted_headers1)
            assert status==200 and json.loads(body)['data']['deleted'] is True
            # GET image returns 404
            status,body,_=req(base+'/api/v1/images/'+uid,headers=restarted_headers1)
            assert status==404,(status,body)
            # Catalog list decreased from 6 to 5
            status,body,_=req(base+'/api/v1/images',headers=restarted_headers1)
            assert len(json.loads(body)['data'])==5
            # Manifest CSV excludes deleted image
            status,manifest_body,_=req(base+'/api/v1/export/manifest.csv',headers=restarted_headers1)
            assert uid.encode() not in manifest_body

            assert (Path(temp)/'morfo.sqlite3').exists()
            print('MORFO smoke PASS: auth, isolation, progressive 49 MP pyramid, experimental analysis, append-only review, conflict, restart and CRUD')

        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired: proc.kill()

if __name__=='__main__': main()
